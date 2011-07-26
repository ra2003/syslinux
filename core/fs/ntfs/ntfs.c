/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2011 Paulo Alcantara <pcacjr@gmail.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, Inc., 53 Temple Place Ste 330,
 *   Boston MA 02111-1307, USA; either version 2 of the License, or
 *   (at your option) any later version; incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

/*
 * ntfs.c - The NTFS filesystem functions
 */

#include <dprintf.h>
#include <stdio.h>
#include <string.h>
#include <sys/dirent.h>
#include <cache.h>
#include <core.h>
#include <disk.h>
#include <fs.h>
#include <ilog2.h>
#include <klibc/compiler.h>

#include "codepage.h"
#include "ntfs.h"

/* Check if there are specific zero fields in an NTFS boot sector */
static inline int ntfs_check_zero_fields(const struct ntfs_bpb *sb)
{
    return !sb->res_sectors && (!sb->zero_0[0] && !sb->zero_0[1] &&
            !sb->zero_0[2]) && !sb->zero_1 && !sb->zero_2 &&
            !sb->zero_3;
}

static inline int ntfs_check_sb_fields(const struct ntfs_bpb *sb)
{
    return ntfs_check_zero_fields(sb) &&
            (!memcmp(sb->oem_name, "NTFS    ", 8) ||
             !memcmp(sb->oem_name, "MSWIN4.0", 8) ||
             !memcmp(sb->oem_name, "MSWIN4.1", 8));
}

static inline struct inode *new_ntfs_inode(struct fs_info *fs)
{
    struct inode *inode;

    inode = alloc_inode(fs, 0, sizeof(struct ntfs_inode));
    if (!inode)
        malloc_error("inode structure");

    return inode;
}

static inline const void *get_right_block(struct fs_info *fs,
                                                        block_t block)
{
    return get_cache(fs->fs_dev, NTFS_SB(fs)->mft_block + block);
}

static int fixups_copyback(struct fs_info *fs, void *buf, NTFS_RECORD *nrec,
                            uint64_t size)
{
    uint8_t *usa_start;
    uint16_t usa_no;
    uint8_t *usa_end;
    unsigned sectors;
    uint64_t offset = 0;
    uint16_t *val;
    const unsigned byte_shift = 8;

    if (nrec->magic != NTFS_MAGIC_FILE && nrec->magic != NTFS_MAGIC_INDX) {
        printf("Not a NTFS record\n");
        goto out;
    }

    /* sectors per block */
    sectors = size >> SECTOR_SHIFT(fs);

    /* get the Update Sequence Array start address */
    usa_start = (uint8_t *)nrec + nrec->usa_ofs;
    /* get the Update Sequence Number */
    usa_no = *(uint16_t *)usa_start;
    /* get the Update Sequence Array end address */
    usa_end = usa_start + nrec->usa_count + 1;

    do {
        offset += SECTOR_SIZE(fs) - 2;

        val = (uint16_t *)((uint8_t *)buf + offset);
        /* compare it against the last two bytes of every sector */
        if (*val == usa_no) {
            if (usa_start < usa_end && usa_start + 1 < usa_end) {
                *val = *usa_start++;
                *val <<= byte_shift | *usa_start++;
            }
        }
    } while ((offset + SECTOR_SIZE(fs)) >> SECTOR_SHIFT(fs) < sectors);

    return 0;

out:
    return -1;
}

static int64_t mft_record_lookup(uint32_t file, struct fs_info *fs,
                                    block_t *block, void *data)
{
    int64_t offset = 0;
    const uint8_t *ret;
    int err;
    const uint64_t blk_size = UINT64_C(1) << BLOCK_SHIFT(fs);
    const uint64_t mft_record_size = NTFS_SB(fs)->mft_record_size;
    MFT_RECORD *mrec;

    goto jump_in;

    for (;;) {
        err = fixups_copyback(fs, (uint8_t *)data + offset,
                                (NTFS_RECORD *)data, mft_record_size);
        if (err)
            break;

        mrec = (MFT_RECORD *)((uint8_t *)data + offset);
        if (mrec->mft_record_no == file)
            return offset;   /* MFT record found! */

        offset += mrec->bytes_allocated;
        if (offset >= BLOCK_SIZE(fs)) {
            ++*block;
            offset -= BLOCK_SIZE(fs);
jump_in:
            ret = get_right_block(fs, *block);
            if (!ret)
                break;

            memcpy(data, ret, blk_size);
        }
    }

    return -1;
}

static ATTR_RECORD *attr_lookup(uint32_t type, const MFT_RECORD *mrec)
{
    ATTR_RECORD *attr;

    /* sanity check */
    if (!mrec || type == NTFS_AT_END)
        return NULL;

    attr = (ATTR_RECORD *)((uint8_t *)mrec + mrec->attrs_offset);
    /* walk through the file attribute records */
    for (;; attr = (ATTR_RECORD *)((uint8_t *)attr + attr->len)) {
        if (attr->type == NTFS_AT_END)
            return NULL;

        if (attr->type == type)
            break;
    }

    return attr;
}

static bool ntfs_match_longname(const char *str, unsigned long mft_no,
                                    struct fs_info *fs)
{
    uint8_t data[1 << BLOCK_SHIFT(fs)];
    int64_t offset;
    MFT_RECORD *mrec;
    block_t block = 0;
    ATTR_RECORD *attr;
    FILE_NAME_ATTR *fn;
    uint8_t len;
    unsigned char c = -1;	/* Nonzero: we have not yet seen NULL */
    uint16_t cp;
    const uint16_t *match;

    dprintf("Matching: %s\n", str);

    offset = mft_record_lookup(mft_no, fs, &block, &data);
    if (offset < 0) {
        printf("No MFT record found!\n");
        goto out;
    }

    mrec = (MFT_RECORD *)&data[offset];

    attr = attr_lookup(NTFS_AT_FILENAME, mrec);
    if (!attr) {
        printf("No attribute found!\n");
        goto out;
    }

    fn = (FILE_NAME_ATTR *)((uint8_t *)attr + attr->data.resident.value_offset);
    len = fn->file_name_len;
    match = fn->file_name;

    /*
    if (!strcmp("syslinux", str)) {
        const char *s = (const char *)fn->file_name;
        printf("Filename:                   ");
        while (*s) {
            printf("%c", *s);
            s += 2;
        }
        printf("\n");
    }
    */

    while (len) {
        cp = *match++;
        len--;
        if (!cp)
            break;

        c = *str++;
        if (cp != codepage.uni[0][c] && cp != codepage.uni[1][c])
            goto out;
    }

    if (*str)
        goto out;

    while (len--)
        if (*match++ != 0xffff)
            goto out;

    return true;

out:
    return false;
}

enum {
    MAP_UNSPEC,
    MAP_START           = 1 << 0,
    MAP_END             = 1 << 1,
    MAP_ALLOCATED       = 1 << 2,
    MAP_UNALLOCATED     = 1 << 3,
    MAP_MASK            = 0x0000000F,
};

struct mapping_chunk {
    uint64_t cur_vcn;   /* Current Virtual Cluster Number */
    uint8_t vcn_len;    /* Virtual Cluster Number length in bytes */
    uint64_t next_vcn;  /* Next Virtual Cluster Number */
    uint8_t lcn_len;    /* Logical Cluster Number length in bytes */
    int64_t cur_lcn;    /* Logical Cluster Number offset */
    uint32_t flags;     /* Specific flags of this chunk */
};

/* Parse data runs.
 *
 * return 0 on success or -1 on failure.
 */
static int parse_data_run(const void *stream, uint32_t *offset,
                            uint8_t *attr_len, struct mapping_chunk *chunk)
{
    uint8_t *buf;   /* Pointer to the zero-terminated byte stream */
    uint8_t count;  /* The count byte */
    uint8_t v, l;   /* v is the number of changed low-order VCN bytes;
                     * l is the number of changed low-order LCN bytes
                     */
    uint8_t *byte;
    int byte_shift = 8;
    int mask;
    uint8_t val;
    int64_t res;

    chunk->flags &= ~MAP_MASK;

    buf = (uint8_t *)stream + *offset;
    if (buf > attr_len || !*buf) {
        chunk->flags |= MAP_END;    /* we're done */
        return 0;
    }

    if (!*offset)
        chunk->flags |= MAP_START;  /* initial chunk */

    chunk->cur_vcn = chunk->next_vcn;

    count = *buf;
    v = count & 0x0F;
    l = count >> 4;

    if (v > 8 || l > 8) /* more than 8 bytes ? */
        goto out;

    chunk->vcn_len = v;
    chunk->lcn_len = l;

    byte = (uint8_t *)buf + v;
    count = v;

    res = 0LL;
    while (count--) {
        val = *byte--;
        mask = val >> (byte_shift - 1);
        res = (res << byte_shift) | ((val + mask) ^ mask);
    }

    chunk->next_vcn += res;

    byte = (uint8_t *)buf + v + l;
    count = l;

    mask = 0xFFFFFFFF;
    res = 0LL;
    if (*byte & 0x80)
        res |= (int64_t)mask;   /* sign-extend it */

    while (count--)
        res = (res << byte_shift) | *byte--;

    chunk->cur_lcn += res;
    if (!chunk->cur_lcn) {  /* is LCN 0 ? */
        /* then VCNS from cur_vcn to next_vcn - 1 are unallocated */
        chunk->flags |= MAP_UNALLOCATED;
    } else {
        /* otherwise they're all allocated */
        chunk->flags |= MAP_ALLOCATED;
    }

    *offset += v + l + 1;

    return 0;

out:
    return -1;
}

static enum dirent_type get_inode_mode(MFT_RECORD *mrec)
{
    ATTR_RECORD *attr;
    FILE_NAME_ATTR *fn;
    bool infile = false;
    uint32_t dir_mask, root_mask, file_mask;
    uint32_t dir, root, file;

    attr = attr_lookup(NTFS_AT_FILENAME, mrec);
    if (!attr) {
        dprintf("No attribute found!\n");
        return DT_UNKNOWN;
    }

    fn = (FILE_NAME_ATTR *)((uint8_t *)attr +
                                attr->data.resident.value_offset);
    dprintf("File attributes:        0x%X\n", fn->file_attrs);

    dir_mask = NTFS_FILE_ATTR_ARCHIVE |
                NTFS_FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT;
    root_mask = NTFS_FILE_ATTR_READONLY | NTFS_FILE_ATTR_HIDDEN |
                NTFS_FILE_ATTR_SYSTEM |
                NTFS_FILE_ATTR_DUP_FILE_NAME_INDEX_PRESENT;
    file_mask = NTFS_FILE_ATTR_ARCHIVE;

    dir = fn->file_attrs & ~dir_mask;
    root = fn->file_attrs & ~root_mask;
    file = fn->file_attrs & ~file_mask;

    dprintf("dir = 0x%X\n", dir);
    dprintf("root= 0x%X\n", root);
    dprintf("file = 0x%X\n", file);
    if (((!dir && root) || (!dir && !root)) && !file)
        infile = true;

    return infile ? DT_REG : DT_DIR;
}

static int index_inode_setup(struct fs_info *fs, unsigned long mft_no,
                                struct inode *inode)
{
    uint8_t data[1 << BLOCK_SHIFT(fs)];
    int64_t offset;
    MFT_RECORD *mrec;
    block_t block = 0;
    ATTR_RECORD *attr;
    enum dirent_type d_type;
    uint32_t len;
    INDEX_ROOT *ir;
    uint32_t clust_size;
    uint8_t *attr_len;
    struct mapping_chunk chunk;
    int err;
    uint8_t *stream;
    uint32_t droffset;

    offset = mft_record_lookup(mft_no, fs, &block, &data);
    if (offset < 0) {
        dprintf("No MFT record found!\n");
        goto out;
    }

    mrec = (MFT_RECORD *)&data[offset];

    NTFS_PVT(inode)->mft_no = mft_no;
    NTFS_PVT(inode)->seq_no = mrec->seq_no;

    NTFS_PVT(inode)->start_cluster = block >> NTFS_SB(fs)->clust_shift;
    NTFS_PVT(inode)->here = block;

    d_type = get_inode_mode(mrec);
    if (d_type == DT_UNKNOWN) {
        dprintf("Failed on determining inode's mode\n");
        goto out;
    }

    if (d_type == DT_DIR) {    /* directory stuff */
        printf("Got a directory.\n");
        attr = attr_lookup(NTFS_AT_INDEX_ROOT, mrec);
        if (!attr) {
            dprintf("No attribute found!\n");
            goto out;
        }

        printf("here!\n");

        /* note: INDEX_ROOT is always resident */
        ir = (INDEX_ROOT *)((uint8_t *)attr +
                                    attr->data.resident.value_offset);
        len = attr->data.resident.value_len;
        if ((uint8_t *)ir + len > (uint8_t *)mrec +
                        NTFS_SB(fs)->mft_record_size) {
            dprintf("Corrupt index\n");
            goto out;
        }

        NTFS_PVT(inode)->itype.index.collation_rule = ir->collation_rule;
        NTFS_PVT(inode)->itype.index.block_size = ir->index_block_size;
        NTFS_PVT(inode)->itype.index.block_size_shift =
                            ilog2(NTFS_PVT(inode)->itype.index.block_size);

        /* determine the size of a vcn in the index */
        clust_size = NTFS_PVT(inode)->itype.index.block_size;
        if (NTFS_SB(fs)->clust_size <= clust_size) {
            NTFS_PVT(inode)->itype.index.vcn_size = NTFS_SB(fs)->clust_size;
            NTFS_PVT(inode)->itype.index.vcn_size_shift =
                                        NTFS_SB(fs)->clust_shift;
        } else {
            NTFS_PVT(inode)->itype.index.vcn_size = BLOCK_SIZE(fs);
            NTFS_PVT(inode)->itype.index.vcn_size_shift = BLOCK_SHIFT(fs);
        }
    } else if (d_type == DT_REG) {        /* file stuff */
        printf("Got a file.\n");
        attr = attr_lookup(NTFS_AT_DATA, mrec);
        if (!attr) {
            dprintf("No attribute found!\n");
            goto out;
        }

        NTFS_PVT(inode)->non_resident = attr->non_resident;
        NTFS_PVT(inode)->type = attr->type;

        if (!attr->non_resident) {
            NTFS_PVT(inode)->data.resident.offset =
                (uint32_t)((uint8_t *)attr + attr->data.resident.value_offset);
            inode->size = attr->data.resident.value_len;
        } else {
            attr_len = (uint8_t *)attr + attr->len;

            memset((void *)&chunk, 0, sizeof(chunk));
            chunk.cur_vcn = attr->data.non_resident.lowest_vcn;
            chunk.cur_lcn = 0LL;

            stream = (uint8_t *)attr +
                            attr->data.non_resident.mapping_pairs_offset;
            droffset = 0U;

            for (;;) {
                err = parse_data_run(stream, &droffset, attr_len, &chunk);
                if (err) {
                    printf("Non-resident $DATA attribute without any run\n");
                    goto out;
                }

                if (chunk.flags & MAP_UNALLOCATED)
                    continue;
                if (chunk.flags & (MAP_ALLOCATED | MAP_END))
                    break;
            }

            if (chunk.flags & MAP_END) {
                dprintf("No mapping found\n");
                goto out;
            }

            NTFS_PVT(inode)->data.non_resident.start_vcn = chunk.cur_vcn;
            NTFS_PVT(inode)->data.non_resident.next_vcn = chunk.next_vcn;
            NTFS_PVT(inode)->data.non_resident.vcn_no = chunk.vcn_len;
            NTFS_PVT(inode)->data.non_resident.lcn = chunk.cur_lcn;
            inode->size = attr->data.non_resident.initialized_size;
        }
    }

    inode->mode = d_type;

    return 0;

out:
    return -1;
}

static struct inode *index_lookup(const char *dname, struct inode *dir)
{
    struct fs_info *fs = dir->fs;
    const uint64_t blk_size = UINT64_C(1) << BLOCK_SHIFT(fs);
    uint8_t data[blk_size];
    int64_t offset;
    MFT_RECORD *mrec;
    block_t block;
    ATTR_RECORD *attr;
    INDEX_ROOT *ir;
    uint32_t len;
    INDEX_ENTRY *ie;
    uint8_t vcn_count;
    uint8_t *ret;
    INDEX_BLOCK *iblock;
    int64_t vcn;
    uint8_t *stream;
    uint8_t *attr_len;
    struct mapping_chunk chunk;
    uint32_t droffset;
    int err;
    struct inode *inode;

    block = NTFS_PVT(dir)->start;
    dprintf("index_lookup() - mft record number: %d\n", NTFS_PVT(dir)->mft_no);
    offset = mft_record_lookup(NTFS_PVT(dir)->mft_no, fs, &block, &data);
    if (offset < 0) {
        dprintf("No MFT record found!\n");
        goto out;
    }

    mrec = (MFT_RECORD *)&data[offset];

    attr = attr_lookup(NTFS_AT_INDEX_ROOT, mrec);
    if (!attr) {
        dprintf("No attribute found!\n");
        goto out;
    }

    ir = (INDEX_ROOT *)((uint8_t *)attr +
                            attr->data.resident.value_offset);
    len = attr->data.resident.value_len;
    /* sanity check */
    if ((uint8_t *)ir + len > (uint8_t *)mrec + NTFS_SB(fs)->mft_record_size)
        goto index_err;

    ie = (INDEX_ENTRY *)((uint8_t *)&ir->index +
                                ir->index.entries_offset);
    for (;; ie = (INDEX_ENTRY *)((uint8_t *)ie + ie->len)) {
        /* bounds checks */
        if ((uint8_t *)ie < (uint8_t *)mrec ||
            (uint8_t *)ie + sizeof(INDEX_ENTRY_HEADER) >
            (uint8_t *)&ir->index + ir->index.index_len ||
            (uint8_t *)ie + ie->len >
            (uint8_t *)&ir->index + ir->index.index_len)
            goto index_err;

        /* last entry cannot contain a key. it can however contain
         * a pointer to a child node in the B+ tree so we just break out
         */
        dprintf("(0) ie->flags:          0x%X\n", ie->flags);
        if (ie->flags & INDEX_ENTRY_END)
            break;

        if (ntfs_match_longname(dname, ie->data.dir.indexed_file, fs)) {
            dprintf("Filename matches up!\n");
            dprintf("MFT record number = %d\n", ie->data.dir.indexed_file);
            goto found;
        }
    }

    /* check for the presence of a child node */
    if (!(ie->flags & INDEX_ENTRY_NODE)) {
        dprintf("No child node, aborting...\n");
        goto out;
    }

    /* then descend into child node */

    attr = attr_lookup(NTFS_AT_INDEX_ALLOCATION, mrec);
    if (!attr) {
        printf("No attribute found!\n");
        goto out;
    }

    if (!attr->non_resident) {
        printf("WTF ?! $INDEX_ALLOCATION isn't really resident.\n");
        goto out;
    }

    attr_len = (uint8_t *)attr + attr->len;

    memset((void *)&chunk, 0, sizeof(chunk));
    chunk.cur_vcn = attr->data.non_resident.lowest_vcn;
    chunk.cur_lcn = 0LL;

    stream = (uint8_t *)attr + attr->data.non_resident.mapping_pairs_offset;
    droffset = 0U;

    for (;;) {
        err = parse_data_run(stream, &droffset, attr_len, &chunk);
        if (err)
            goto not_found;

        if (chunk.flags & MAP_UNALLOCATED)
            continue;
        if (chunk.flags & MAP_END)
            break;

        if (chunk.flags & MAP_ALLOCATED) {
            chunk.cur_lcn = 0x24de8;
            printf("%d cluster(s) starting at 0x%X\n", chunk.vcn_len,
                    chunk.cur_lcn);

            vcn_count = 0;
            vcn = chunk.cur_vcn;        /* HUMMMMMMM!!!!! */
            while (vcn_count++ < chunk.vcn_len) {
                block = ((chunk.cur_lcn + vcn) << NTFS_SB(fs)->clust_shift) <<
                        SECTOR_SHIFT(fs) >> BLOCK_SHIFT(fs);

                ret = (uint8_t *)get_cache(fs->fs_dev, block);
                if (!ret) {
                    printf("get_cache() returned NULL\n");
                    goto not_found;
                }

                memcpy(data, ret, blk_size);

                err = fixups_copyback(fs, data, (NTFS_RECORD *)data, blk_size);
                if (err)
                    goto not_found;

                iblock = (INDEX_BLOCK *)data;
                if (iblock->magic != NTFS_MAGIC_INDX) {
                    printf("Not a valid INDX record\n");
                    goto not_found;
                }

                ie = (INDEX_ENTRY *)((uint8_t *)&iblock->index +
                                            iblock->index.entries_offset);
                for (;; ie = (INDEX_ENTRY *)((uint8_t *)ie + ie->len)) {
                    /* bounds checks */
                    if ((uint8_t *)ie < (uint8_t *)iblock || (uint8_t *)ie +
                        sizeof(INDEX_ENTRY_HEADER) >
                        (uint8_t *)&iblock->index + iblock->index.index_len ||
                        (uint8_t *)ie + ie->len >
                        (uint8_t *)&iblock->index + iblock->index.index_len)
                        goto index_err;

                    /* last entry cannot contain a key */
                    if (ie->flags & INDEX_ENTRY_END)
                        break;

                    if (ntfs_match_longname(dname, ie->data.dir.indexed_file,
                                            fs)) {
                        dprintf("Filename matches up!\n");
                        dprintf("MFT record number = %d\n",
                                ie->data.dir.indexed_file);
                        goto found;
                    }
                }

                ++vcn;  /* go to the next VCN */
            }
        }
    }

not_found:
    dprintf("Index not found\n");

out:
    printf("%s not found!\n", dname);

    return NULL;

found:
    dprintf("--------------- Found index -------------------\n");
    inode = new_ntfs_inode(fs);
    err = index_inode_setup(fs, ie->data.dir.indexed_file, inode);
    if (err) {
        free(inode);
        goto out;
    }

    printf("%s found!\n", dname);

    return inode;

index_err:
    printf("Corrupt index. Aborting lookup...\n");
    goto out;
}

/*
 * Convert an UTF-16LE longname to the system codepage; return
 * the length on success or -1 on failure.
 */
static int ntfs_cvt_longname(char *entry_name, const uint16_t *long_name)
{
    struct unicache {
        uint16_t utf16;
        uint8_t cp;
    };
    static struct unicache unicache[256];
    struct unicache *uc;
    uint16_t cp;
    unsigned int c;
    char *p = entry_name;

    do {
        cp = *long_name++;
        uc = &unicache[cp % 256];

        if (__likely(uc->utf16 == cp)) {
            *p++ = uc->cp;
        } else {
            for (c = 0; c < 512; c++) {
                if (codepage.uni[0][c] == cp) {
                    uc->utf16 = cp;
                    *p++ = uc->cp = (uint8_t)c;
                    goto found;
                }
            }

            return -1;
            found:
                ;
        }
    } while (cp);

    return (p - entry_name) - 1;
}

static int ntfs_next_extent(struct inode *inode, uint32_t lstart)
{
    struct fs_info *fs = inode->fs;
    struct ntfs_sb_info *sbi = NTFS_SB(fs);
    uint32_t mcluster = lstart >> sbi->clust_shift;
    uint32_t tcluster;
    const uint32_t cluster_bytes = UINT32_C(1) << sbi->clust_byte_shift;
    sector_t pstart;
    const uint32_t sec_size = SECTOR_SIZE(fs);
    const uint32_t sec_shift = SECTOR_SHIFT(fs);

    tcluster = (inode->size + cluster_bytes - 1) >> sbi->clust_byte_shift;
    if (mcluster >= tcluster)
        goto out;       /* Requested cluster beyond end of file */

    if (!NTFS_PVT(inode)->non_resident) {
        pstart = sbi->mft_block + NTFS_PVT(inode)->here;
        pstart <<= BLOCK_SHIFT(fs) >> sec_shift;
    } else {
        pstart = NTFS_PVT(inode)->data.non_resident.lcn << sbi->clust_shift;
    }

    inode->next_extent.len = (inode->size + sec_size - 1) >> sec_shift;
    inode->next_extent.pstart = pstart;

    return 0;

out:
    return -1;
}

static uint32_t ntfs_getfssec(struct file *file, char *buf, int sectors,
                                bool *have_more)
{
    uint8_t non_resident;
    uint32_t ret;
    int64_t offset;
    struct fs_info *fs = file->fs;
    uint8_t data[1 << BLOCK_SHIFT(fs)];
    struct inode *inode = file->inode;
    block_t block = 0;
    MFT_RECORD *mrec;
    ATTR_RECORD *attr;
    char *p;
    int err;

    non_resident = NTFS_PVT(inode)->non_resident;

    ret = generic_getfssec(file, buf, sectors, have_more);
    if (!ret)
        return ret;

    if (!non_resident) {
        dprintf("mft_no:     %d\n", NTFS_PVT(inode)->mft_no);
        offset = mft_record_lookup(NTFS_PVT(inode)->mft_no, fs, &block, &data);
        if (offset < 0) {
            dprintf("No MFT record found!\n");
            goto out;
        }

        mrec = (MFT_RECORD *)&data[offset];

        attr = attr_lookup(NTFS_AT_DATA, mrec);
        if (!attr) {
            dprintf("No attribute found!\n");
            goto out;
        }

        p = (char *)attr + attr->data.resident.value_offset;

        /* p now points to the data offset, so let's copy it into buf */
        memcpy(buf, p, inode->size);

        err = fixups_copyback(fs, buf, (NTFS_RECORD *)mrec, inode->size);
        if (err)
            goto out;

        ret = inode->size;
    }

    return ret;

out:
    return 0;
}

static int ntfs_readdir(struct file *file, struct dirent *dirent)
{
    int64_t offset;
    struct fs_info *fs = file->fs;
    uint8_t data[1 << BLOCK_SHIFT(fs)];
    MFT_RECORD *mrec;
    struct inode *inode = file->inode;
    block_t block = 0;
    ATTR_RECORD *attr;
    FILE_NAME_ATTR *fn;
    char filename[NTFS_MAX_FILE_NAME_LEN + 1];
    int len;

    printf("in readdir()\n");

    offset = mft_record_lookup(NTFS_PVT(inode)->mft_no, fs, &block, &data);
    if (offset < 0) {
        dprintf("No MFT record found!\n");
        goto out;
    }

    mrec = (MFT_RECORD *)&data[offset];

    attr = attr_lookup(NTFS_AT_FILENAME, mrec);
    if (!attr) {
        dprintf("No attribute found!\n");
        goto out;
    }

    fn = (FILE_NAME_ATTR *)((uint8_t *)attr +
                            attr->data.resident.value_offset);

    len = ntfs_cvt_longname(filename, fn->file_name);
    if (len < 0 || len != fn->file_name_len) {
        dprintf("Failed on converting UTF-16LE LFN to OEM LFN\n");
        goto out;
    }

    dirent->d_ino = NTFS_PVT(inode)->mft_no;
    dirent->d_off = file->offset;
    dirent->d_reclen = offsetof(struct dirent, d_name) + len + 1;
    dirent->d_type = get_inode_mode(mrec);
    memcpy(dirent->d_name, filename, len + 1);

    return 0;

out:
    return -1;
}

static struct inode *ntfs_iget(const char *dname, struct inode *parent)
{
    return index_lookup(dname, parent);
}

static struct inode *ntfs_iget_root(struct fs_info *fs)
{
    struct inode *inode = new_ntfs_inode(fs);
    int err;

    inode->fs = fs;

    err = index_inode_setup(fs, FILE_root, inode);
    if (err)
        goto free_out;

    NTFS_PVT(inode)->start = NTFS_PVT(inode)->here;

    return inode;

free_out:
    free(inode);

    return NULL;
}

/* Initialize the filesystem metadata and return block size in bits */
static int ntfs_fs_init(struct fs_info *fs)
{
    struct ntfs_bpb ntfs;
    struct ntfs_sb_info *sbi;
    struct disk *disk = fs->fs_dev->disk;
    uint8_t clust_per_mft_record;

    disk->rdwr_sectors(disk, &ntfs, 0, 1, 0);

    /* sanity check */
    if (!ntfs_check_sb_fields(&ntfs))
        return -1;

    /* Note: clust_per_mft_record can be a negative number */
    clust_per_mft_record = ntfs.clust_per_mft_record < 0 ?
                    -ntfs.clust_per_mft_record : ntfs.clust_per_mft_record;

    SECTOR_SHIFT(fs) = disk->sector_shift;

    /* We need _at least_ 1 KiB to read the whole MFT record */
    BLOCK_SHIFT(fs) = ilog2(ntfs.sec_per_clust) + SECTOR_SHIFT(fs);
    if (BLOCK_SHIFT(fs) < clust_per_mft_record)
        BLOCK_SHIFT(fs) = clust_per_mft_record;

    SECTOR_SIZE(fs) = 1 << SECTOR_SHIFT(fs);
    BLOCK_SIZE(fs) = 1 << BLOCK_SHIFT(fs);

    sbi = malloc(sizeof(*sbi));
    if (!sbi)
        malloc_error("ntfs_sb_info structure");

    fs->fs_info = sbi;

    sbi->clust_shift        = ilog2(ntfs.sec_per_clust);
    sbi->clust_byte_shift   = sbi->clust_shift + SECTOR_SHIFT(fs);
    sbi->clust_mask         = ntfs.sec_per_clust - 1;
    sbi->clust_size         = ntfs.sec_per_clust << SECTOR_SHIFT(fs);
    sbi->mft_record_size    = 1 << clust_per_mft_record;

    sbi->mft_block = ntfs.mft_lclust << sbi->clust_shift <<
                    SECTOR_SHIFT(fs) >> BLOCK_SHIFT(fs);
    /* 16 MFT entries reserved for metadata files (approximately 16 KiB) */
    sbi->mft_size = (clust_per_mft_record << sbi->clust_shift) << 4;

    sbi->clusters = ntfs.total_sectors << SECTOR_SHIFT(fs) >> sbi->clust_shift;
    if (sbi->clusters > 0xFFFFFFFFFFF4ULL)
        sbi->clusters = 0xFFFFFFFFFFF4ULL;

    /* Initialize the cache */
    cache_init(fs->fs_dev, BLOCK_SHIFT(fs));

    return BLOCK_SHIFT(fs);
}

const struct fs_ops ntfs_fs_ops = {
    .fs_name        = "ntfs",
    .fs_flags       = FS_USEMEM | FS_THISIND,
    .fs_init        = ntfs_fs_init,
    .searchdir      = NULL,
    .getfssec       = ntfs_getfssec,
    .close_file     = generic_close_file,
    .mangle_name    = generic_mangle_name,
    .load_config    = generic_load_config,
    .readdir        = ntfs_readdir,
    .iget_root      = ntfs_iget_root,
    .iget           = ntfs_iget,
    .next_extent    = ntfs_next_extent,
};
