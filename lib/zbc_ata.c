/*
 * This file is part of libzbc.
 *
 * Copyright (C) 2009-2014, HGST, Inc.
 * All rights reserved.
 *
 * This software is distributed
 * under the terms of the BSD 2-clause license, "as is," without
 * technical support, and WITHOUT ANY WARRANTY, without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. You should have received a copy of the BSD 2-clause
 * license along with libzbc. If not, see  <http://opensource.org/licenses/BSD-2-Clause>.
 *
 * Authors: Damien Le Moal (damien.lemoal@hgst.com)
 */

/***** Including files *****/

#include "zbc.h"
#include "zbc_sg.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/***** Macro definitions *****/

/**
 * Number of bytes in a Zone Descriptor.
 */
#define ZBC_ZONE_DESCRIPTOR_LENGTH		64

/**
 * Number of bytes in the buffer before the first Zone Descriptor.
 */
#define ZBC_ZONE_DESCRIPTOR_OFFSET		64

/**
 * ATA commands.
 */
#define ZBC_ATA_IDENTIFY             		0xEC
#define ZBC_ATA_EXEC_DEV_DIAGNOSTIC		0x90
#define ZBC_ATA_READ_LOG_DMA_EXT		0x47
#define ZBC_ATA_SET_FEATURES                    0xEF
#define ZBC_ATA_REQUEST_SENSE_DATA_EXT          0x0B
#define ZBC_ATA_READ_DMA_EXT			0x25
#define ZBC_ATA_WRITE_DMA_EXT			0x35
#define ZBC_ATA_FLUSH_CACHE_EXT			0xEA
#define ZBC_ATA_ZAC_MANAGEMENT_IN               0x4A
#define ZBC_ATA_ZAC_MANAGEMENT_OUT              0x9F

/**
 * Set features feature fieald.
 */
#define ENABLE_SENSE_DATA_REPORTING             0xC3

/**
 * ZAC management commands
 */
/**
 * Report zones command
 */
#define ZBC_ATA_REPORT_ZONES_EXT_CMD            ZBC_ATA_ZAC_MANAGEMENT_IN
#define ZBC_ATA_REPORT_ZONES_EXT_AF             0x00

/**
 * Close zone command
 */
#define ZBC_ATA_CLOSE_ZONE_EXT_CMD              ZBC_ATA_ZAC_MANAGEMENT_OUT
#define ZBC_ATA_CLOSE_ZONE_EXT_AF               0x01

/**
 * Finish zone command
 */
#define ZBC_ATA_FINISH_ZONE_EXT_CMD             ZBC_ATA_ZAC_MANAGEMENT_OUT
#define ZBC_ATA_FINISH_ZONE_EXT_AF              0x02

/**
 * Open zone command
 */
#define ZBC_ATA_OPEN_ZONE_EXT_CMD               ZBC_ATA_ZAC_MANAGEMENT_OUT
#define ZBC_ATA_OPEN_ZONE_EXT_AF                0x03

/**
 * Reset write pointer command
 */
#define ZBC_ATA_RESET_WRITE_POINTER_EXT_CMD     ZBC_ATA_ZAC_MANAGEMENT_OUT
#define ZBC_ATA_RESET_WRITE_POINTER_EXT_AF      0x04

/**
 * Zoned device infomation page
 */
#define ZBC_ATA_IDENTIFY_DEVICE_DATA_LOG_ADDRESS 0x30
#define ZBC_ATA_ZONED_DEVICE_INFORMATION_PAGE    0x09

/**
 * For a disk connected to an AHCI port, the kernel libata/SCSI layer
 * handles command translation. So native SCSI read/write commands
 * can be sent to the disk. However, in the case of a SAS HBA connected disk,
 * native SCSI read/write commands may not be translated by the HBA for HM disks.
 * This device flag indicates that the disk accepts native SCSI commands,
 * which is tested when the device is open.
 */
#define ZBC_ATA_SCSI_RW                         0x00000001

/***** Definition of private functions *****/

/**
 * Get a word from a command data buffer.
 */
static inline uint16_t
zbc_ata_get_word(uint8_t *buf)
{
    return( (uint16_t)buf[0]
	    | ((uint16_t)buf[1] << 8) );
}

/**
 * Get a Dword from a command data buffer.
 */
static inline uint32_t
zbc_ata_get_dword(uint8_t *buf)
{
    return( (uint32_t)buf[0]
	    | ((uint32_t)buf[1] << 8)
	    | ((uint32_t)buf[2] << 16)
	    | ((uint32_t)buf[3] << 24) );
}

/**
 * Get a Qword from a command data buffer.
 */
static inline uint64_t
zbc_ata_get_qword(uint8_t *buf)
{
    return( (uint64_t)buf[0]
	    | ((uint64_t)buf[1] << 8)
	    | ((uint64_t)buf[2] << 16)
	    | ((uint64_t)buf[3] << 24)
	    | ((uint64_t)buf[4] << 32)
	    | ((uint64_t)buf[5] << 40)
	    | ((uint64_t)buf[6] << 48)
	    | ((uint64_t)buf[7] << 56) );
}

/**
 * Read log pages.
 */
static int
zbc_ata_read_log(zbc_device_t *dev,
		 uint8_t log,
		 int page,
		 uint8_t *buf,
		 size_t bufsz)
{
    unsigned int lba_count = bufsz / 512;
    zbc_sg_cmd_t cmd;
    int ret;

    /* Intialize command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, buf, bufsz);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                            count (15:8)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                            count (7:0)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          LBA (31:24 (15:8 if ext == 0)                |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          LBA (39:32)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          LBA (47:40)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                           Device                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                           Command                                     |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                           Control                                     |
     * +=============================================================================+
     */
    cmd.io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x6 << 1) | 0x01;	/* DMA protocol, ext=1 */
    cmd.cdb[2] = 0x0e; 			/* off_line=0, ck_cond=0, t_type=0, t_dir=1, byt_blk=1, t_length=10 */
    cmd.cdb[5] = (lba_count >> 8) & 0xff;
    cmd.cdb[6] = lba_count & 0xff;
    cmd.cdb[8] = log;
    cmd.cdb[9] = (page >> 8) & 0xff;
    cmd.cdb[10] = page & 0xff;
    cmd.cdb[14] = ZBC_ATA_READ_LOG_DMA_EXT;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Forward declaration.
 */
static int
zbc_ata_nr_zones(zbc_device_t *dev,
		 uint64_t start_lba,
		 enum zbc_reporting_options ro);

/**
 * Test device signature (return device model detected).
 */
static int
zbc_ata_classify(zbc_device_t *dev)
{
    zbc_sg_cmd_t cmd;
    uint8_t *desc;
    int ret;

    /* Intialize command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, NULL, 0);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                            count (15:8)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                            count (7:0)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          LBA (31:24 15:8)                             |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          LBA (39:32)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          LBA (47:40)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                           Device                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                           Command                                     |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                           Control                                     |
     * +=============================================================================+
     */
    /* Note: According to SAT-3r07, the protocol should be 0x8. */
    /* But if it is used, the SG/SCSI driver returns an error.  */
    /* So use non-data protocol... Also note that to get the    */
    /* device signature, the "check condition" bit must be set. */
    cmd.io_hdr.dxfer_direction = SG_DXFER_NONE;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x3 << 1) | 0x1;	/* Non-Data protocol, ext=1 */
    cmd.cdb[2] = 0x1 << 5;		/* off_line=0, ck_cond=1, t_type=0, t_dir=0, byt_blk=0, t_length=00 */
    cmd.cdb[14] = ZBC_ATA_EXEC_DEV_DIAGNOSTIC;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);
    if ( ret != 0 ) {
	goto out;
    }

    /* It worked, so we can safely assume that this is an ATA device */
    dev->zbd_info.zbd_type = ZBC_DT_ATA;

    /* Test device signature */
    desc = &cmd.sense_buf[8];

    zbc_debug("Device signature is %02x:%02x\n",
	      desc[9],
	      desc[11]);

    if ( (desc[9] == 0xCD) & (desc[11] == 0xAB) ) {

	/* ZAC host-managed signature */
	zbc_debug("Host-managed ZAC signature detected\n");
	dev->zbd_info.zbd_model = ZBC_DM_HOST_MANAGED;

    } else if ( (desc[9] == 0x00) & (desc[11] == 0x00) ) {

	/* Normal device signature: it may be a host-aware device */
	/* So check log page 1Ah to see if there are zones.       */
	zbc_debug("Standard ATA signature detected\n");
	ret = zbc_ata_nr_zones(dev, 0, ZBC_RO_ALL);
	if ( ret == 0 ) {
	    /* No zones: standard or drive managed disk */
	    zbc_debug("Standard or drive managed ATA device detected\n");
	    dev->zbd_info.zbd_model = ZBC_DM_DRIVE_MANAGED;
	    ret = -ENXIO;
	} else if ( ret > 0 ) {
	    /* We have zones: host-aware disk */
	    zbc_debug("Host aware ATA device detected\n");
	    dev->zbd_info.zbd_model = ZBC_DM_HOST_AWARE;
	    ret = 0;
	} else {
            ret = -ENXIO;
        }

    } else {

	/* Unsupported device */
	zbc_debug("Unsupported device (signature %02x:%02x)\n",
		  desc[9],
		  desc[11]);
	ret = -ENXIO;

    }

out:

    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Set features
 */
static int
zbc_ata_set_features(zbc_device_t *dev,
                     uint8_t      feature,
                     uint8_t      count)
{
    zbc_sg_cmd_t cmd;
    int ret;

    /* Intialize command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, NULL, 0);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          n/a                                          |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                          n/a                                          |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                          count (7:0)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          n/a                                          |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          n/a                                          |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          n/a                                          |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |            DEVICE (7:4)           |          LBA(27:24)               |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                          Command                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                          Control                                      |
     * +=============================================================================+
     */
    /* Note: According to SAT-3r07, the protocol should be 0x8. */
    /* But if it is used, the SG/SCSI driver returns an error.  */
    /* So use non-data protocol... Also note that to get the    */
    /* device signature, the "check condition" bit must be set. */
    cmd.io_hdr.dxfer_direction = SG_DXFER_NONE;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = 0x3 << 1;	/* Non-Data protocol */
    cmd.cdb[4] = feature;
    cmd.cdb[6] = count;
    cmd.cdb[14] = ZBC_ATA_SET_FEATURES;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);

    zbc_sg_cmd_destroy(&cmd);

    return( ret );


}

/**
 * Set features
 */
static int
zbc_ata_enable_sense_data(zbc_device_t *dev)
{

    return( zbc_ata_set_features(dev, ENABLE_SENSE_DATA_REPORTING, 1) );

}

/**
 * Sense data is enabled
 */
static inline int
zbc_ata_sense_data_enabled(zbc_sg_cmd_t *cmd)
{
    int ret = 0;

    if ( cmd->io_hdr.sb_len_wr > 8 ) {
        if ( cmd->sense_buf[8] == 0x09          /* Descriptor code */
             && cmd->sense_buf[21] & 0x02 ) {   /* Status including sense data flag */
            ret = 1;
        }
    }

    return( ret );
}

/**
 * Request sense data ext
 */
static int
zbc_ata_request_sense_data_ext(zbc_device_t *dev)
{
    zbc_sg_cmd_t cmd;
    int ret;

    /* Intialize command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, NULL, 0);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                            count (15:8)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                            count (7:0)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          LBA (31:24 15:8)                             |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          LBA (39:32)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          LBA (47:40)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                           Device                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                           Command                                     |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                           Control                                     |
     * +=============================================================================+
     */
    /* Note: According to SAT-3r07, the protocol should be 0x8. */
    /* But if it is used, the SG/SCSI driver returns an error.  */
    /* So use non-data protocol... Also note that to get the    */
    /* device signature, the "check condition" bit must be set. */
    cmd.io_hdr.dxfer_direction = SG_DXFER_NONE;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x3 << 1) | 0x01;	/* Non-Data protocol, ext=1 */
    cmd.cdb[2] = 0x1 << 5;		/* off_line=0, ck_cond=1, t_type=0, t_dir=0, byt_blk=0, t_length=00 */
    cmd.cdb[14] = ZBC_ATA_REQUEST_SENSE_DATA_EXT;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);
    if ( ret == 0 ) {

        if ( cmd.io_hdr.sb_len_wr ) {
            zbc_debug("%s: Sense data (%d B):\n",
                    dev->zbd_filename,
                    cmd.io_hdr.sb_len_wr);
	    zbc_sg_print_bytes(dev, cmd.sense_buf, cmd.io_hdr.sb_len_wr);
        } else {
            zbc_debug("%s: No sense data\n", dev->zbd_filename);
        }

        if ( cmd.io_hdr.sb_len_wr > 8 ) {
	    zbc_debug("%s: Sense key is 0x%x\n",
		      dev->zbd_filename,
		      cmd.sense_buf[19] & 0xF);
	    zbc_debug("%s: Additional sense code is 0x%02x\n",
		      dev->zbd_filename,
		      cmd.sense_buf[17]);
	    zbc_debug("%s: Additional sense code qualifier is 0x%02x\n",
		      dev->zbd_filename,
		      cmd.sense_buf[15]);
            dev->zbd_errno.sk = cmd.sense_buf[19] & 0xF;
            dev->zbd_errno.asc_ascq = ((int)cmd.sense_buf[17] << 8) | (int)cmd.sense_buf[15];
        } else {
            zbc_debug("%s: Sense buffer length is %d (less than 8B)\n",
		      dev->zbd_filename,
		      cmd.io_hdr.sb_len_wr);
        }

    } else {

        zbc_error("REQUEST SENSE DATA command failed\n");

    }

    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Get disk vendor, product ID and revision.
 */
static void
zbc_ata_vendor_id(zbc_device_t *dev)
{
    uint8_t buf[512];
    int n, ret;

    /* Use inquiry. We could use log 30h page 05h (ATA strings) here... */
    ret = zbc_sg_cmd_inquiry(dev, buf);
    if ( ret == 0 ) {

        /* Vendor identification */
        n = zbc_sg_cmd_strcpy(&dev->zbd_info.zbd_vendor_id[0], (char *)&buf[8], 8);

        /* Product identification */
        n += zbc_sg_cmd_strcpy(&dev->zbd_info.zbd_vendor_id[n], (char *)&buf[16], 16);

        /* Product revision */
        n += zbc_sg_cmd_strcpy(&dev->zbd_info.zbd_vendor_id[n], (char *)&buf[32], 4);

    } else {

        zbc_debug("Device inquiry failed %d\n", ret);
        strcpy(&dev->zbd_info.zbd_vendor_id[0], "UNKNOWN");

    }

    return;

}

/**
 * Get the disk capacity and sector size.
 */
static int
zbc_ata_get_capacity(zbc_device_t *dev)
{
    zbc_sg_cmd_t cmd;
    int logical_per_physical;
    int ret;

    /* Initialize the command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_READ_CAPACITY, NULL, ZBC_SG_READ_CAPACITY_REPLY_LEN);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB */
    cmd.cdb[0] = ZBC_SG_READ_CAPACITY_CDB_OPCODE;
    cmd.cdb[1] = ZBC_SG_READ_CAPACITY_CDB_SA;
    zbc_sg_cmd_set_int32(&cmd.cdb[10], ZBC_SG_READ_CAPACITY_REPLY_LEN);

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);
    if ( ret != 0 ) {
        goto out;
    }

    dev->zbd_info.zbd_logical_blocks = zbc_sg_cmd_get_int64(&cmd.out_buf[0]) + 1;
    dev->zbd_info.zbd_logical_block_size = zbc_sg_cmd_get_int32(&cmd.out_buf[8]);
    logical_per_physical = 1 << cmd.out_buf[13] & 0x0f;

    /* Check */
    if ( dev->zbd_info.zbd_logical_block_size <= 0 ) {
        zbc_error("%s: invalid logical sector size\n",
                  dev->zbd_filename);
        ret = -EINVAL;
        goto out;
    }

    if ( ! dev->zbd_info.zbd_logical_blocks ) {
        zbc_error("%s: invalid capacity (logical blocks)\n",
                  dev->zbd_filename);
        ret = -EINVAL;
        goto out;
    }

    dev->zbd_info.zbd_physical_block_size = dev->zbd_info.zbd_logical_block_size * logical_per_physical;
    dev->zbd_info.zbd_physical_blocks = dev->zbd_info.zbd_logical_blocks / logical_per_physical;

out:

    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Get zoned block device information(Maximum or optimul number of opening zones).
 */
static int
zbc_ata_get_zbd_info(zbc_device_t *dev)
{

    uint8_t buf[512];
    int ret;

    /* Get zoned blockd device information */
    ret = zbc_ata_read_log(dev,
                           ZBC_ATA_IDENTIFY_DEVICE_DATA_LOG_ADDRESS,
	                   ZBC_ATA_ZONED_DEVICE_INFORMATION_PAGE,
		           buf,
		           sizeof(buf));
    if ( ret < 0 ) {
        return( ret );
    }

    /* URSWRZ (unrestricted read write sequential required zone) flag */
    dev->zbd_info.zbd_flags |= (zbc_ata_get_qword(&buf[8]) & 0x01) ? ZBC_UNRESTRICTED_READ : 0;

    /* Maximum number of zones for resource management */
    dev->zbd_info.zbd_opt_nr_open_seq_pref = zbc_ata_get_qword(&buf[24]) & 0xffffffff;
    dev->zbd_info.zbd_opt_nr_non_seq_write_seq_pref = zbc_ata_get_qword(&buf[32]) & 0xffffffff;
    dev->zbd_info.zbd_max_nr_open_seq_req = zbc_ata_get_qword(&buf[40]) & 0xffffffff;

    if ( (dev->zbd_info.zbd_model == ZBC_DM_HOST_MANAGED)
	 && (dev->zbd_info.zbd_max_nr_open_seq_req <= 0) ) {
        zbc_error("%s: invalid maximum number of open sequential write required zones for host-managed device\n",
                  dev->zbd_filename);
        ret = -EINVAL;
    }

    return( ret );

}

/**
 * Get a device information (capacity & sector sizes).
 */
static int
zbc_ata_get_info(zbc_device_t *dev)
{
    int ret;

    /* Make sure the device is ready */
    ret = zbc_sg_cmd_test_unit_ready(dev);
    if ( ret != 0 ) {
        return( ret );
    }

    /* Get device model */
    ret = zbc_ata_classify(dev);
    if ( ret != 0 ) {
        return( ret );
    }

    /* Get vendor information */
    zbc_ata_vendor_id(dev);

    /* Get capacity information */
    ret = zbc_ata_get_capacity(dev);
    if ( ret != 0 ) {
        return( ret );
    }

    /* Get zoned block device information */
    ret = zbc_ata_get_zbd_info(dev);
    if ( ret != 0 ) {
        return( ret );
    }

    return( 0 );

}

/**
 * Read from a ZAC device using READ DMA EXT packed in an ATA PASSTHROUGH command.
 */
static int32_t
zbc_ata_pread_ata(zbc_device_t *dev,
                  zbc_zone_t *zone,
                  void *buf,
                  uint32_t lba_count,
                  uint64_t lba_ofst)
{
    size_t sz = (size_t) lba_count * dev->zbd_info.zbd_logical_block_size;
    uint64_t lba = zone->zbz_start + lba_ofst;
    zbc_sg_cmd_t cmd;
    int ret;

    /* Check */
    if ( lba_count > 65536 ) {
	zbc_error("Read operation too large (limited to 65536 x 512 B sectors)\n");
        return( -EINVAL );
    }

    /* Initialize the command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, buf, sz);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                            count (15:8)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                            count (7:0)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          LBA (31:24 (15:8 if ext == 0)                |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          LBA (39:32)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          LBA (47:40)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                           Device                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                           Command                                     |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                           Control                                     |
     * +=============================================================================+
     */
    cmd.io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x6 << 1) | 0x01;	/* DMA protocol, ext=1 */
    cmd.cdb[2] = 0x0e;			/* off_line=0, ck_cond=0, t_type=0, t_dir=1, byt_blk=1, t_length=10 */
    cmd.cdb[5] = (lba_count >> 8) & 0xff;
    cmd.cdb[6] = lba_count & 0xff;
    cmd.cdb[7] = (lba >> 24) & 0xff;
    cmd.cdb[8] = lba & 0xff;
    cmd.cdb[9] = (lba >> 32) & 0xff;
    cmd.cdb[10] = (lba >> 8) & 0xff;
    cmd.cdb[11] = (lba >> 40) & 0xff;
    cmd.cdb[12] = (lba >> 16) & 0xff;
    cmd.cdb[13] = 1 << 6;
    cmd.cdb[14] = ZBC_ATA_READ_DMA_EXT;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);
    if ( ret == 0 ) {
        ret = (sz - cmd.io_hdr.resid) / dev->zbd_info.zbd_logical_block_size;
    } else {
        /* Request sense data */
        if ( ret == -EIO ) {
            if ( zbc_ata_sense_data_enabled(&cmd) ) {
                zbc_ata_request_sense_data_ext(dev);
            }
        }
    }

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Read from a ZAC device using native SCSI command.
 */
static int32_t
zbc_ata_pread_scsi(zbc_device_t *dev,
                   zbc_zone_t *zone,
                   void *buf,
                   uint32_t lba_count,
                   uint64_t lba_ofst)
{
    size_t sz = (size_t) lba_count * dev->zbd_info.zbd_logical_block_size;
    zbc_sg_cmd_t cmd;
    int ret;

    /* READ 16 */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_READ, buf, sz);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB */
    cmd.cdb[0] = ZBC_SG_READ_CDB_OPCODE;
    cmd.cdb[1] = 0x10;
    zbc_sg_cmd_set_int64(&cmd.cdb[2], (zone->zbz_start + lba_ofst));
    zbc_sg_cmd_set_int32(&cmd.cdb[10], lba_count);

    /* Send the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);
    if ( ret == 0 ) {
        ret = (sz - cmd.io_hdr.resid) / dev->zbd_info.zbd_logical_block_size;
    }

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Read from a ZAC device.
 */
static int32_t
zbc_ata_pread(zbc_device_t *dev,
              zbc_zone_t *zone,
              void *buf,
              uint32_t lba_count,
              uint64_t lba_ofst)
{
    int ret;

    /* ATA command or native SCSI command ? */
    if ( dev->zbd_flags & ZBC_ATA_SCSI_RW ) {
        ret = zbc_ata_pread_scsi(dev, zone, buf, lba_count, lba_ofst);
    } else {
        ret = zbc_ata_pread_ata(dev, zone, buf, lba_count, lba_ofst);
    }

    return( ret );

}

/**
 * Write to a ZAC device using WRITE DMA EXT packed in an ATA PASSTHROUGH command.
 */
static int32_t
zbc_ata_pwrite_ata(zbc_device_t *dev,
                   zbc_zone_t *zone,
                   const void *buf,
                   uint32_t lba_count,
                   uint64_t lba_ofst)
{
    size_t sz = (size_t) lba_count * dev->zbd_info.zbd_logical_block_size;
    uint64_t lba = zone->zbz_start + lba_ofst;
    zbc_sg_cmd_t cmd;
    int ret;

    /* Check */
    if ( lba_count > 65536 ) {
	zbc_error("Write operation too large (limited to 65536 x 512 B sectors)\n");
        return( -EINVAL );
    }

    /* Initialize the command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, (uint8_t *)buf, sz);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                           count (15:8)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                           count (7:0)                                 |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                           LBA (31:24)                                 |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                            LBA (7:0)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                           LBA (39:32)                                 |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                           LBA (15:8)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                           LBA (47:40)                                 |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                           LBA (23:16)                                 |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                             Device                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                             Command                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                             Control                                   |
     * +=============================================================================+
     */
    cmd.io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x6 << 1) | 0x01;	/* DMA protocol, ext=1 */
    cmd.cdb[2] = 0x06;			/* off_line=0, ck_cond=0, t_type=1, t_dir=0, byt_blk=1, t_length=10 */
    cmd.cdb[5] = (lba_count >> 8) & 0xff;
    cmd.cdb[6] = lba_count & 0xff;
    cmd.cdb[7] = (lba >> 24) & 0xff;
    cmd.cdb[8] = lba & 0xff;
    cmd.cdb[9] = (lba >> 32) & 0xff;
    cmd.cdb[10] = (lba >> 8) & 0xff;
    cmd.cdb[11] = (lba >> 40) & 0xff;
    cmd.cdb[12] = (lba >> 16) & 0xff;
    cmd.cdb[13] = 1 << 6;
    cmd.cdb[14] = ZBC_ATA_WRITE_DMA_EXT;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);
    if ( ret == 0 ) {
        ret = (sz - cmd.io_hdr.resid) / dev->zbd_info.zbd_logical_block_size;
    } else {
        /* Request sense data */
        if ( ret == -EIO ) {
            if ( zbc_ata_sense_data_enabled(&cmd) ) {
                zbc_ata_request_sense_data_ext(dev);
            }
        }
    }

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Write to a ZAC device using native SCSI commands.
 */
static int32_t
zbc_ata_pwrite_scsi(zbc_device_t *dev,
                    zbc_zone_t *zone,
                    const void *buf,
                    uint32_t lba_count,
                    uint64_t lba_ofst)
{
    size_t sz = (size_t) lba_count * dev->zbd_info.zbd_logical_block_size;
    zbc_sg_cmd_t cmd;
    int ret;

    /* WRITE 16 */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_WRITE, (uint8_t *)buf, sz);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB */
    cmd.cdb[0] = ZBC_SG_WRITE_CDB_OPCODE;
    cmd.cdb[1] = 0x10;
    zbc_sg_cmd_set_int64(&cmd.cdb[2], (zone->zbz_start + lba_ofst));
    zbc_sg_cmd_set_int32(&cmd.cdb[10], lba_count);

    /* Send the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);
    if ( ret == 0 ) {
        ret = (sz - cmd.io_hdr.resid) / dev->zbd_info.zbd_logical_block_size;
    }

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Write to a ZAC device.
 */
static int32_t
zbc_ata_pwrite(zbc_device_t *dev,
               zbc_zone_t *zone,
               const void *buf,
               uint32_t lba_count,
               uint64_t lba_ofst)
{
    int ret;
    /* ATA command or native SCSI command ? */
    if ( dev->zbd_flags & ZBC_ATA_SCSI_RW ) {
        ret = zbc_ata_pwrite_scsi(dev, zone, buf, lba_count, lba_ofst);
    } else {
        ret = zbc_ata_pwrite_ata(dev, zone, buf, lba_count, lba_ofst);
    }

    return( ret );

}

/**
 * Flush a ZAC device cache.
 */
static int
zbc_ata_flush(zbc_device_t *dev,
	      uint64_t lba_ofst,
	      uint32_t lba_count,
	      int immediate)
{
    zbc_sg_cmd_t cmd;
    int ret;

    /* Initialize the command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, NULL, 0);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB */
    cmd.io_hdr.dxfer_direction = SG_DXFER_NONE;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x3 << 1) | 0x01;		/* Non-Data protocol, ext=1 */
    cmd.cdb[14] = ZBC_ATA_FLUSH_CACHE_EXT;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

#define ZBC_ATA_REPORT_ZONES_BUFSZ	524288

/**
 * Get device zone information.
 */
static int
zbc_ata_report_zones(zbc_device_t *dev,
		     uint64_t start_lba,
		     enum zbc_reporting_options ro,
		     uint64_t *max_lba,
		     zbc_zone_t *zones,
		     unsigned int *nr_zones)
{
    size_t bufsz = ZBC_ZONE_DESCRIPTOR_OFFSET;
    unsigned int i, nz, buf_nz;
    zbc_sg_cmd_t cmd;
    uint8_t *buf;
    int ret;

    if ( *nr_zones ) {
        bufsz += (size_t)*nr_zones * ZBC_ZONE_DESCRIPTOR_LENGTH;
    }

    bufsz = (bufsz + 511) & ~511;
    bufsz = (bufsz + 4095) & ~4095;
    if ( bufsz > ZBC_ATA_REPORT_ZONES_BUFSZ ) {
	bufsz = ZBC_ATA_REPORT_ZONES_BUFSZ;
    }

    /* Allocate and intialize report zones command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, NULL, bufsz);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                            count (15:8)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                            count (7:0)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          LBA (31:24 (15:8 if ext == 0)                |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          LBA (39:32)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          LBA (47:40)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                           Device                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                           Command                                     |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                           Control                                     |
     * +=============================================================================+
     */
    cmd.io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x06 << 1) | 0x01;	/* DMA protocol, ext=1 */
    cmd.cdb[2] = 0x0e; 			/* off_line=0, ck_cond=0, t_type=0, t_dir=1, byt_blk=1, t_length=10 */
    cmd.cdb[3] = ro;
    cmd.cdb[4] = ZBC_ATA_REPORT_ZONES_EXT_AF;
    cmd.cdb[5] = ((bufsz / 512) >> 8) & 0xff;
    cmd.cdb[6] = (bufsz / 512) & 0xff;
    cmd.cdb[8]  = start_lba & 0xff;
    cmd.cdb[10] = (start_lba >>  8) & 0xff;
    cmd.cdb[12] = (start_lba >> 16) & 0xff;
    cmd.cdb[7]  = (start_lba >> 24) & 0xff;
    cmd.cdb[9]  = (start_lba >> 32) & 0xff;
    cmd.cdb[11] = (start_lba >> 40) & 0xff;
    cmd.cdb[13] = 1 << 6;
    cmd.cdb[14] = ZBC_ATA_REPORT_ZONES_EXT_CMD;

    /* Send the SG_IO command */
    ret = zbc_sg_cmd_exec(dev, &cmd);
    if ( ret != 0 ) {
        if ( ret == -EIO ) {
	    /* Request sense data */
            if ( (dev->zbd_errno.sk != ZBC_E_ILLEGAL_REQUEST)
                 && (dev->zbd_errno.asc_ascq != ZBC_E_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE) ) {
                /* If request sense data is enabled , try to get sense data */
                if ( zbc_ata_sense_data_enabled(&cmd) ) {
                    zbc_ata_request_sense_data_ext(dev);
                }
            }
        }
        goto out;
    }

    if ( cmd.out_bufsz < ZBC_ZONE_DESCRIPTOR_OFFSET ) {
        zbc_error("Not enough data received (need at least %d B, got %zu B)\n",
                  ZBC_ZONE_DESCRIPTOR_OFFSET,
                  cmd.out_bufsz);
        ret = -EIO;
        goto out;
    }

    /* Get number of zones in result */
    buf = (uint8_t *) cmd.out_buf;
    nz = zbc_ata_get_dword(buf) / ZBC_ZONE_DESCRIPTOR_LENGTH;
    if ( max_lba ) {
	*max_lba = zbc_ata_get_qword(&buf[8]);
    }

    if ( zones && nz ) {

        /* Get zone info */
        if ( nz > *nr_zones ) {
            nz = *nr_zones;
        }

	buf_nz = (cmd.out_bufsz - ZBC_ZONE_DESCRIPTOR_OFFSET) / ZBC_ZONE_DESCRIPTOR_LENGTH;
        if ( nz > buf_nz ) {
            nz = buf_nz;
        }

        /* Get zone descriptors */
	buf += ZBC_ZONE_DESCRIPTOR_OFFSET;
        for(i = 0; i < nz; i++) {

            zones[i].zbz_type = buf[0] & 0x0f;
            zones[i].zbz_condition = (buf[1] >> 4) & 0x0f;
            zones[i].zbz_length = zbc_ata_get_qword(&buf[8]);
            zones[i].zbz_start = zbc_ata_get_qword(&buf[16]);
            zones[i].zbz_write_pointer = zbc_ata_get_qword(&buf[24]);
            zones[i].zbz_flags = buf[1] & 0x03;

            buf += ZBC_ZONE_DESCRIPTOR_LENGTH;

        }

    }

    /* Return number of zones */
    *nr_zones = nz;

out:

    /* Cleanup */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Get the number of zones of the disk.
 */
static int
zbc_ata_nr_zones(zbc_device_t *dev,
		 uint64_t start_lba,
		 enum zbc_reporting_options ro)
{
    unsigned int nr_zones;
    int ret;

    /* Get general purpose log */
    ret = zbc_ata_report_zones(dev, start_lba, ro, NULL, NULL, &nr_zones);
    if ( ret == 0 ) {
	ret = nr_zones;
    }

    return( ret );

}

/**
 * Open zone(s).
 */
static int
zbc_ata_open_zone(zbc_device_t *dev,
                  uint64_t start_lba)
{
    zbc_sg_cmd_t cmd;
    int ret;

    /* Intialize command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, NULL, 0);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                            count (15:8)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                            count (7:0)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          LBA (31:24 (15:8 if ext == 0)                |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          LBA (39:32)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          LBA (47:40)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                           Device                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                           Command                                     |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                           Control                                     |
     * +=============================================================================+
     */
    cmd.io_hdr.dxfer_direction = SG_DXFER_NONE;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x3 << 1) | 0x01;	/* Non-Data protocol, ext=1 */
    cmd.cdb[4] = ZBC_ATA_OPEN_ZONE_EXT_AF;
    if ( start_lba == (uint64_t)-1 ) {
        /* Open all zones */
        cmd.cdb[3] = 0x01;
    } else {
        /* Open only the zone at start_lba */
	cmd.cdb[8] = start_lba & 0xff;
	cmd.cdb[10] = (start_lba >> 8) & 0xff;
	cmd.cdb[12] = (start_lba >> 16) & 0xff;
	cmd.cdb[7] = (start_lba >> 24) & 0xff;
	cmd.cdb[9] = (start_lba >> 32) & 0xff;
	cmd.cdb[11] = (start_lba >> 40) & 0xff;
    }
    cmd.cdb[13] = 1 << 6;
    cmd.cdb[14] = ZBC_ATA_OPEN_ZONE_EXT_CMD;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);

    /* Request sense data */
    if ( ret == -EIO ) {
        if ( zbc_ata_sense_data_enabled(&cmd) ) {
            zbc_ata_request_sense_data_ext(dev);
        }
    }

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Close zone(s).
 */
static int
zbc_ata_close_zone(zbc_device_t *dev,
                   uint64_t start_lba)
{
    zbc_sg_cmd_t cmd;
    int ret;

    /* Intialize command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, NULL, 0);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                            count (15:8)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                            count (7:0)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          LBA (31:24 (15:8 if ext == 0)                |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          LBA (39:32)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          LBA (47:40)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                           Device                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                           Command                                     |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                           Control                                     |
     * +=============================================================================+
     */
    cmd.io_hdr.dxfer_direction = SG_DXFER_NONE;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x3 << 1) | 0x01;	/* Non-Data protocol, ext=1 */
    cmd.cdb[4] = ZBC_ATA_CLOSE_ZONE_EXT_AF;
    if ( start_lba == (uint64_t)-1 ) {
        /* Close all zones */
        cmd.cdb[3] = 0x01;
    } else {
        /* Close only the zone at start_lba */
	cmd.cdb[8] = start_lba & 0xff;
	cmd.cdb[10] = (start_lba >> 8) & 0xff;
	cmd.cdb[12] = (start_lba >> 16) & 0xff;
	cmd.cdb[7] = (start_lba >> 24) & 0xff;
	cmd.cdb[9] = (start_lba >> 32) & 0xff;
	cmd.cdb[11] = (start_lba >> 40) & 0xff;
    }
    cmd.cdb[13] = 1 << 6;
    cmd.cdb[14] = ZBC_ATA_CLOSE_ZONE_EXT_CMD;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);

    /* Request sense data */
    if ( ret == -EIO ) {
        if ( zbc_ata_sense_data_enabled(&cmd) ) {
            zbc_ata_request_sense_data_ext(dev);
        }
    }

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Finish zone(s).
 */
static int
zbc_ata_finish_zone(zbc_device_t *dev,
                    uint64_t start_lba)
{
    zbc_sg_cmd_t cmd;
    int ret;

    /* Intialize command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, NULL, 0);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                            count (15:8)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                            count (7:0)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          LBA (31:24 (15:8 if ext == 0)                |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          LBA (39:32)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          LBA (47:40)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                           Device                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                           Command                                     |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                           Control                                     |
     * +=============================================================================+
     */
    cmd.io_hdr.dxfer_direction = SG_DXFER_NONE;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x3 << 1) | 0x01;	/* Non-Data protocol, ext=1 */
    cmd.cdb[4] = ZBC_ATA_FINISH_ZONE_EXT_AF;
    if ( start_lba == (uint64_t)-1 ) {
        /* Finish all zones */
        cmd.cdb[3] = 0x01;
    } else {
        /* Finish only the zone at start_lba */
	cmd.cdb[8] = start_lba & 0xff;
	cmd.cdb[10] = (start_lba >> 8) & 0xff;
	cmd.cdb[12] = (start_lba >> 16) & 0xff;
	cmd.cdb[7] = (start_lba >> 24) & 0xff;
	cmd.cdb[9] = (start_lba >> 32) & 0xff;
	cmd.cdb[11] = (start_lba >> 40) & 0xff;
    }
    cmd.cdb[13] = 1 << 6;
    cmd.cdb[14] = ZBC_ATA_FINISH_ZONE_EXT_CMD;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);

    /* Request sense data */
    if ( ret == -EIO ) {
        if ( zbc_ata_sense_data_enabled(&cmd) ) {
            zbc_ata_request_sense_data_ext(dev);
        }
    }

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * Reset zone(s) write pointer.
 */
static int
zbc_ata_reset_write_pointer(zbc_device_t *dev,
			    uint64_t start_lba)
{
    zbc_sg_cmd_t cmd;
    int ret;

    /* Intialize command */
    ret = zbc_sg_cmd_init(&cmd, ZBC_SG_ATA16, NULL, 0);
    if ( ret != 0 ) {
        zbc_error("zbc_sg_cmd_init failed\n");
        return( ret );
    }

    /* Fill command CDB:
     * +=============================================================================+
     * |  Bit|   7    |   6    |   5    |   4    |   3    |   2    |   1    |   0    |
     * |Byte |        |        |        |        |        |        |        |        |
     * |=====+==========================+============================================|
     * | 0   |                           Operation Code (85h)                        |
     * |-----+-----------------------------------------------------------------------|
     * | 1   |      Multiple count      |              Protocol             |  ext   |
     * |-----+-----------------------------------------------------------------------|
     * | 2   |    off_line     |ck_cond | t_type | t_dir  |byt_blk |    t_length     |
     * |-----+-----------------------------------------------------------------------|
     * | 3   |                          features (15:8)                              |
     * |-----+-----------------------------------------------------------------------|
     * | 4   |                          features (7:0)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 5   |                            count (15:8)                               |
     * |-----+-----------------------------------------------------------------------|
     * | 6   |                            count (7:0)                                |
     * |-----+-----------------------------------------------------------------------|
     * | 7   |                          LBA (31:24 (15:8 if ext == 0)                |
     * |-----+-----------------------------------------------------------------------|
     * | 8   |                          LBA (7:0)                                    |
     * |-----+-----------------------------------------------------------------------|
     * | 9   |                          LBA (39:32)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 10  |                          LBA (15:8)                                   |
     * |-----+-----------------------------------------------------------------------|
     * | 11  |                          LBA (47:40)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 12  |                          LBA (23:16)                                  |
     * |-----+-----------------------------------------------------------------------|
     * | 13  |                           Device                                      |
     * |-----+-----------------------------------------------------------------------|
     * | 14  |                           Command                                     |
     * |-----+-----------------------------------------------------------------------|
     * | 15  |                           Control                                     |
     * +=============================================================================+
     */
    cmd.io_hdr.dxfer_direction = SG_DXFER_NONE;
    cmd.cdb[0] = ZBC_SG_ATA16_CDB_OPCODE;
    cmd.cdb[1] = (0x3 << 1) | 0x01;	/* Non-Data protocol, ext=1 */
    cmd.cdb[4] = ZBC_ATA_RESET_WRITE_POINTER_EXT_AF;
    if ( start_lba == (uint64_t)-1 ) {
        /* Reset all zones */
        cmd.cdb[3] = 0x01;
    } else {
        /* Reset only the zone at start_lba */
	cmd.cdb[8] = start_lba & 0xff;
	cmd.cdb[10] = (start_lba >> 8) & 0xff;
	cmd.cdb[12] = (start_lba >> 16) & 0xff;
	cmd.cdb[7] = (start_lba >> 24) & 0xff;
	cmd.cdb[9] = (start_lba >> 32) & 0xff;
	cmd.cdb[11] = (start_lba >> 40) & 0xff;
    }
    cmd.cdb[13] = 1 << 6;
    cmd.cdb[14] = ZBC_ATA_RESET_WRITE_POINTER_EXT_CMD;

    /* Execute the command */
    ret = zbc_sg_cmd_exec(dev, &cmd);

    /* Request sense data */
    if ( ret == -EIO ) {
        if ( zbc_ata_sense_data_enabled(&cmd) ) {
            zbc_ata_request_sense_data_ext(dev);
        }
    }

    /* Done */
    zbc_sg_cmd_destroy(&cmd);

    return( ret );

}

/**
 * If the disk is connected to a SAS HBA, test if command translation is
 * working properly with HM disks (as those do not have a standard device
 * signature, the HBA may fail command translation).
 * In the case of an AHCI connected disks, the kernel libata/SCSI layer
 * will handle the translation.
 * If testing does not complete properly, assume that native SCSI commands are OK.
 */
static int
zbc_ata_scsi_rw(zbc_device_t *dev)
{
    unsigned int nr_zones = 1;
    zbc_zone_t zone;
    void *buf;
    int ret;

    if ( dev->zbd_info.zbd_model == ZBC_DM_HOST_AWARE ) {
        /* SCSI commands should work */
        return( 1 );
    }

    /* Host managed: find a conventional zone */
    ret = zbc_ata_report_zones(dev, 0, ZBC_RO_NOT_WP | ZBC_RO_PARTIAL, NULL, &zone, &nr_zones);
    if ( ret != 0 ) {
        return( 1 );
    }

    if ( ! nr_zones ) {

        /* No conventional zones: try a closed or full sequential zones */
        nr_zones = 1;
        ret = zbc_ata_report_zones(dev, 0, ZBC_RO_CLOSED | ZBC_RO_PARTIAL, NULL, &zone, &nr_zones);
        if ( ret != 0 ) {
            return( 1 );
        }

        if ( ! nr_zones ) {
	    nr_zones = 1;
	    ret = zbc_ata_report_zones(dev, 0, ZBC_RO_FULL | ZBC_RO_PARTIAL, NULL, &zone, &nr_zones);
	    if ( ret != 0 ) {
		return( 1 );
	    }
	}

        if ( ! nr_zones ) {
            zbc_debug("No suitable zone found for r/w test: assuming SCSI r/w commands are OK\n");
            return( 1 );
        }

    }

    zbc_debug("R/W test zone: type 0x%x, cond 0x%x, need_reset %d, non_seq %d, LBA %llu, %llu sectors, wp %llu\n",
              (int)zone.zbz_type,
              (int)zone.zbz_condition,
              zbc_zone_need_reset(&zone),
              zbc_zone_non_seq(&zone),
              (unsigned long long) zone.zbz_start,
              (unsigned long long) zone.zbz_length,
              (unsigned long long) zone.zbz_write_pointer);

    /* Get a buffer for testing */
    buf = malloc(dev->zbd_info.zbd_logical_block_size);
    if ( ! buf ) {
        zbc_error("No memory for r/w test\n");
        return( 1 );
    }

    /* Test SCSI command */
    ret = zbc_ata_pread_scsi(dev, &zone, buf, 1, 0);
    if ( ret > 0 ) {
        ret = 1;
    } else {
        ret = 0;
    }

    free(buf);

    return( ret );

}

static int
zbc_ata_open(const char *filename,
	     int flags,
	     struct zbc_device **pdev)
{
    struct zbc_device *dev;
    struct stat st;
    int fd, ret;

    zbc_debug("%s: ########## Trying ATA driver ##########\n",
	      filename);

    /* Open the device file */
    fd = open(filename, zbc_open_flags(flags));
    if ( fd < 0 ) {
        zbc_error("Open device file %s failed %d (%s)\n",
                  filename,
                  errno,
                  strerror(errno));
        ret = -errno;
	goto out;
    }

    /* Check device */
    if ( fstat(fd, &st) != 0 ) {
        zbc_error("Stat device %s failed %d (%s)\n",
                  filename,
                  errno,
                  strerror(errno));
        ret = -errno;
        goto out;
    }

    if ( (! S_ISCHR(st.st_mode))
         && (! S_ISBLK(st.st_mode)) ) {
        ret = -ENXIO;
        goto out;
    }

    /* Set device decriptor */
    ret = -ENOMEM;
    dev = calloc(1, sizeof(struct zbc_device));
    if ( ! dev ) {
        goto out;
    }

    dev->zbd_filename = strdup(filename);
    if ( ! dev->zbd_filename ) {
        goto out_free_dev;
    }

    dev->zbd_fd = fd;

    ret = zbc_ata_get_info(dev);
    if ( ret ) {
        goto out_free_filename;
    }

    /* Set sense data reporting */
    ret = zbc_ata_enable_sense_data(dev);
    if ( ret ) {
        /* nothing to do */
    }

    /* Test if the disk accepts native SCSI read/write commands */
    if ( (! (flags & ZBC_FORCE_ATA_RW))
	 && zbc_ata_scsi_rw(dev) ) {
	zbc_debug("%s: Using SCSI R/W commands\n",
		  filename);
	dev->zbd_flags |= ZBC_ATA_SCSI_RW;
    } else {
	zbc_debug("%s: Using ATA R/W commands\n",
		  filename);
	dev->zbd_flags &= ~ZBC_ATA_SCSI_RW;
    }

    *pdev = dev;

    zbc_debug("%s: ########## ATA driver succeeded ##########\n",
	      filename);

    return( 0 );

out_free_filename:

    free(dev->zbd_filename);

out_free_dev:

    free(dev);

out:

    if ( fd >= 0 ) {
	close(fd);
    }

    zbc_debug("%s: ########## ATA driver failed %d ##########\n",
	      filename,
	      ret);

    return( ret );

}

static int
zbc_ata_close(zbc_device_t *dev)
{

    if ( close(dev->zbd_fd) ) {
        return( -errno );
    }

    free(dev->zbd_filename);
    free(dev);

    return( 0 );

}

/**
 * ZAC with ATA HDIO operations.
 */
zbc_ops_t zbc_ata_ops =
{
    .zbd_open         = zbc_ata_open,
    .zbd_close        = zbc_ata_close,
    .zbd_pread        = zbc_ata_pread,
    .zbd_pwrite       = zbc_ata_pwrite,
    .zbd_flush        = zbc_ata_flush,
    .zbd_report_zones = zbc_ata_report_zones,
    .zbd_open_zone    = zbc_ata_open_zone,
    .zbd_close_zone   = zbc_ata_close_zone,
    .zbd_finish_zone  = zbc_ata_finish_zone,
    .zbd_reset_wp     = zbc_ata_reset_write_pointer,
};

