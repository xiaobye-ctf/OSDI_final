/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME       "ssd_file"
enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};


static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int lba : 16;
        unsigned int nand: 16;
    } fields;
};

PCA_RULE curr_pca;
static unsigned int get_next_pca();

unsigned int* L2P,* P2L,* valid_count, free_block_number;

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    if (new_size > NAND_SIZE_KB * 1024)
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        return 0;
    }

}

static int ssd_expand(size_t new_size)
{
    //logic must less logic limit

    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    //read
    if ( (fptr = fopen(nand_name, "r") ))
    {
        fseek( fptr, my_pca.fields.lba * 512, SEEK_SET );
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}
static int nand_write(const char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    //write
    if ( (fptr = fopen(nand_name, "r+")))
    {
        fseek( fptr, my_pca.fields.lba * 512, SEEK_SET );
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
        physic_size ++;
        valid_count[my_pca.fields.nand]++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block_index)
{
    char nand_name[100];
    FILE* fptr;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block_index);
    fptr = fopen(nand_name, "w");
    if (fptr == NULL)
    {
        printf("erase nand_%d fail", block_index);
        return 0;
    }
    fclose(fptr);
    valid_count[block_index] = FREE_BLOCK;
    return 1;
}

static unsigned int get_next_block()
{
    for (int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        if (valid_count[(curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM] == FREE_BLOCK)
        {
            curr_pca.fields.nand = (curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM;
            curr_pca.fields.lba = 0;
            free_block_number--;
            valid_count[curr_pca.fields.nand] = 0;
            return curr_pca.pca;
        }
    }
    return OUT_OF_BLOCK;
}
static unsigned int get_next_pca()
{
    if (curr_pca.pca == INVALID_PCA)
    {
        //init
        curr_pca.pca = 0;
        valid_count[0] = 0;
        free_block_number--;
        return curr_pca.pca;
    }

    if(curr_pca.fields.lba == 9)
    {
        int temp = get_next_block();
        if (temp == OUT_OF_BLOCK)
        {
            return OUT_OF_BLOCK;
        }
        else if(temp == -EINVAL)
        {
            return -EINVAL;
        }
        else
        {
            return temp;
        }
    }
    else
    {
        curr_pca.fields.lba += 1;
    }
    return curr_pca.pca;
}


static int ftl_read(char* buf, size_t lba)
{
    // TODO
    PCA_RULE pca;
    pca.pca = L2P[lba];
    printf("ftl_read: lba = %lu, L2P[lba].fields.nand = %hu, L2P[lba].fields.lba = %hu\n", lba, ((PCA_RULE)L2P[lba]).fields.nand, ((PCA_RULE)L2P[lba]).fields.lba);
    if(pca.pca == INVALID_LBA) return 0;
    return nand_read(buf, pca.pca);
}

static void _my_GC(){
    int target_block, target_phy_page, target_logic_page;
    unsigned int min_valid_page_num = PAGE_PER_BLOCK + 1;
    char *tmp_buf = malloc(512);
    PCA_RULE pca, old_pca;
    // pick a block that contain less valid page 
    for(int i = 0 ; i < PHYSICAL_NAND_NUM ; i++){
        if(curr_pca.fields.nand != i && min_valid_page_num >= valid_count[i]){
            min_valid_page_num = valid_count[i];
            target_block = i;
        }
    }

    printf("*****************************************\n");
    printf("Garbage collector pick block %d with %d valid page as target\n", target_block, min_valid_page_num);
    printf("*****************************************\n");


    // copy valid page to free page
    for(int i = 0 ; i < PAGE_PER_BLOCK ; i++){ 
        target_phy_page = i + target_block * PAGE_PER_BLOCK; 

        if(P2L[target_phy_page] != INVALID_PCA){
            target_logic_page = P2L[target_phy_page];
            P2L[target_phy_page] = INVALID_PCA;
        }else{
            continue;
        }

        if((pca.pca = get_next_pca()) == OUT_OF_BLOCK){
            free(tmp_buf);
            return;
        } 
        // update lba's status and copy valid page to free page
        old_pca.pca = L2P[target_logic_page];
        L2P[target_logic_page] = pca.pca;
        P2L[pca.fields.lba + pca.fields.nand * PAGE_PER_BLOCK] = target_logic_page;

        printf("old_pca.fields.nand = %hu, old_pca.fields.lba = %hu\n", old_pca.fields.nand, old_pca.fields.lba);
        printf("pca.fields.nand = %hu, pca.fields.lba = %hu\n", pca.fields.nand, pca.fields.lba);
        nand_read(tmp_buf, old_pca.pca);
        nand_write(tmp_buf, pca.pca);
    }

    // erase target block
    nand_erase(target_block);
    free_block_number++;

    free(tmp_buf);
}

static int ftl_write(const char* buf, size_t lba_range, size_t lba)
{
    // TODO
    PCA_RULE pca;
    PCA_RULE old_pca;
    size_t ret = 0;
    printf("################################################\n");
    printf("ftl_write: lba = %lu, lba_range = %lu\n", lba, lba_range);
    for (int i = 0 ; i < lba_range ; i++, lba++) {
        if( free_block_number <= 1){
            // GC
            _my_GC();
            // if free_block_number is still PHYSICAL_NAND_NUM after executing GC
            // then it means that ssd has no room for the data
            if( free_block_number <= 0) return ENOSPC;
        }

        pca.pca = get_next_pca();
        printf("pca.fields.nand = %hu, pca.fields.lba = %hu\n", pca.fields.nand, pca.fields.lba);

        if(L2P[lba] != INVALID_LBA){
        // update corresponding old pca's status
            old_pca = (PCA_RULE)L2P[lba];
            P2L[old_pca.fields.lba + old_pca.fields.nand * PAGE_PER_BLOCK] = INVALID_PCA; 
            valid_count[old_pca.fields.nand]--;
        }
            
        // update corresponding new pca's status and lba's status
        L2P[lba] = pca.pca;
        P2L[pca.fields.lba + pca.fields.nand * PAGE_PER_BLOCK] = lba; 
        ret += nand_write(buf + i * 512, pca.pca);
    }
    printf("################################################\n");
    return ret;
}



static int ssd_file_type(const char* path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}
static int ssd_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi)
{
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}
static int ssd_open(const char* path, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}
static int ssd_do_read(char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range;
    char* tmp_buf;

    //off limit
    if ((offset ) >= logic_size)
    {
        return 0;
    }
    if ( size > logic_size - offset)
    {
        //is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++) {
        // TODO
        if(tmp_lba + i > LOGICAL_NAND_NUM * PAGE_PER_BLOCK) break;  
        ftl_read(tmp_buf + i * 512, tmp_lba + i);
    }

    memcpy(buf, tmp_buf + offset % 512, size);

    
    free(tmp_buf);
    return size;
}
static int ssd_read(const char* path, char* buf, size_t size,
                    off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}
static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range;
    char* tmp_buf;
    
    printf("ssd_do_write: size = %lu, offset = %lu\n",size ,offset);
    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;

    // TODO
    tmp_buf = (char*)malloc(512);
     
    for (int i = 0; i < tmp_lba_range; i++) {
        if(i == 0 && offset % 512){
            ftl_read(tmp_buf, tmp_lba + i);
            memcpy(tmp_buf, &buf[offset], (size + offset > 512) ? 512 - offset : size);
        }else if(i == 0 && size < 512){
            ftl_read(tmp_buf, tmp_lba + i);
            memcpy(tmp_buf, buf,  size);
        }else if( i == tmp_lba_range - 1 && (offset + size) % 512){
            ftl_read(tmp_buf, tmp_lba + i); 
            memcpy(tmp_buf, &buf[i * 512], 512 - ((offset + size) % 512));
        }else{
            memcpy(tmp_buf, &buf[i * 512], 512);
        }
        if(tmp_lba + i > LOGICAL_NAND_NUM * PAGE_PER_BLOCK) break;  
        if(ftl_write(tmp_buf, 1, tmp_lba + i) == ENOSPC) return ENOSPC;
    }
    free(tmp_buf);
    return size;
}
static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi)
{

    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}
static int ssd_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi)
{
    (void) fi;
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}
static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags)
{
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}
static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
    }
    return -EINVAL;
}
static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};
int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;

    L2P = malloc(LOGICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    P2L = malloc(PHYSICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    valid_count = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);

    //create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
