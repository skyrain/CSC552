#include <stdio.h>
#include <stdlib.h>
//#include "flash.h"
#include "log.h"
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

//------Note: at least 3 free segment------------

//----according to log addr, find the input value --
//----of"u_int block"------------------------------
//--pre knowledge: seg_size is whole number----
//---- of FLASH_SECTORS_PER_BLOCK--------------------
//--??假设 0 为 "u_int block"的最小值------
u_int find_wear_bk(LogAddress * log_addr)
{
    u_int wear_bk_no = 0;

    //identify the log_addr is which sector (start from 1 not 0)
    u_int sec_offset = log_addr->seg_no * seg_size
        + log_addr->bk_no * bk_size + 1;

    while(sec_offset > 0)
    {
        wear_bk_no ++;
        sec_offset = sec_offset - FLASH_SECTORS_PER_BLOCK;
    }

    return wear_bk_no;
}

//-------judege whehter the block is below wearlimit or not---
bool is_in_wearlimit(LogAddress * log_addr)
{
    u_int wear_bk_no = find_wear_bk(log_addr);

    Flash_Flags flags = FLASH_SILENT;
    //blocks : # of blocks in the flash
    u_int tmp = sec_num / FLASH_SECTORS_PER_BLOCK;
    u_int * blocks = &tmp;
    Flash   flash = Flash_Open(fl_file, flags, blocks);
    u_int * wear = calloc(1, sizeof(u_int)); 
    Flash_GetWear(flash, wear_bk_no, wear);
    Flash_Close(flash);

    bool return_value = true;
    if(*wear > wearlimit)
        return_value = false;

    return return_value;
}

//----judge whether need to change the checkpoint location--
//--requitement: super_seg already intialized--------------
bool need_change_cp_loc()
{
    LogAddrList * cp_addr_walker = calloc(1, sizeof(LogAddrList));
    cp_addr_walker->log_addr.seg_no = super_seg->cp_addr->log_addr.seg_no;
    cp_addr_walker->log_addr.bk_no = super_seg->cp_addr->log_addr.bk_no;

    LogAddress * tmp_log_addr = calloc(1, sizeof(LogAddress));
    tmp_log_addr->seg_no = cp_addr_walker->log_addr.seg_no;
    tmp_log_addr->bk_no = cp_addr_walker->log_addr.bk_no;
    u_int checkpoint_size = super_seg->checkpoint_size;

    bool return_value = false;
    while(checkpoint_size > 0)
    {
        if(!is_in_wearlimit(tmp_log_addr))
        {
            return_value = true;
            break;
        }
        else
        {
            checkpoint_size--;
            //--move to check next cp bk
            if(tmp_log_addr->bk_no == bks_per_seg - 1)
            {
                cp_addr_walker = cp_addr_walker->next;
                tmp_log_addr->seg_no = cp_addr_walker->log_addr.seg_no;
                tmp_log_addr->bk_no = cp_addr_walker->log_addr.bk_no;
            }
            else
            {
                tmp_log_addr->bk_no++;
            }
        }
    }

    return return_value;
}


//----call this func in LFS.c 's main()-----------
//----use this func before use find_free_seg---------
void get_checkpoint_to_memory()
{
    u_int i;

    void * buffer = calloc(1, super_seg->checkpoint_size * bk_size * FLASH_SECTOR_SIZE);
    Flash_Flags flags = FLASH_SILENT;
    //blocks : # of blocks in the flash
    u_int tmp = sec_num / FLASH_SECTORS_PER_BLOCK;
    u_int * blocks = &tmp;
    Flash   flash = Flash_Open(fl_file, flags, blocks); 
    
    LogAddrList * cp_addr_walker = super_seg->cp_addr; 
    u_int buffer_offset = 0;
    while(cp_addr_walker != NULL)
    {
        void * tmp_buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);
        u_int sec_offset = cp_addr_walker->log_addr.seg_no * bks_per_seg;
        Flash_Read(flash, sec_offset, seg_size, tmp_buffer);
        memcpy(buffer + buffer_offset, tmp_buffer, seg_size * FLASH_SECTOR_SIZE);
        buffer_offset += seg_size * FLASH_SECTOR_SIZE;
        free(tmp_buffer);

        cp_addr_walker = cp_addr_walker->next;
    }
    Flash_Close(flash);
    
    //-----cast buffer to checkpoint type-----------
    checkpoint = (Checkpoint *)buffer;

    u_int bytes_offset = sizeof(Checkpoint);    
    checkpoint->ifile = (Inode *)calloc(1, sizeof(Inode));
    memcpy(checkpoint->ifile , (Inode *)(buffer + bytes_offset), sizeof(Inode));
    bytes_offset += sizeof(Inode);

    //-------for seg usage table---------------
    Seg_usage_table * sut_walker = (Seg_usage_table *)calloc(1, sizeof(Seg_usage_table));
    checkpoint->seg_usage_table = sut_walker;

    for(i = 1; i < seg_num; i++)
    {
        Seg_usage_table * sut = (Seg_usage_table *)(buffer + bytes_offset);
        bytes_offset += sizeof(Seg_usage_table);
        if(i != seg_num -1 )
            sut->next = buffer + bytes_offset;
        else
            sut->next = NULL;
        
        memcpy(sut_walker, sut, sizeof(Seg_usage_table));
        sut_walker = sut_walker->next;
    }
    
    //--------for last log addr---------
    checkpoint->last_log_addr = (LogAddress *)calloc(1, sizeof(LogAddress));
    memcpy(checkpoint->last_log_addr, buffer + bytes_offset, sizeof(LogAddress)); 
}

//---find free seg (except the seg tail_log_addr now using)----
//---input: initial finding seg_no, not include itself to be checked---
//--return value: seg_no------------
u_int find_free_seg(u_int start_seg_no)
{
    Seg_usage_table * sut_walker = checkpoint->seg_usage_table;
   
    bool flag = true;
    while(flag)
    {
        if(start_seg_no == seg_num -1)
            start_seg_no = 1;
        else
            start_seg_no++;
        
        u_int sut_seg_no = start_seg_no;
        while(sut_seg_no > 1)
        {
            sut_walker = sut_walker->next;
            sut_seg_no--;
        }

        if(sut_walker->num_live_bk == 0)
            return start_seg_no;
        
        sut_walker = checkpoint->seg_usage_table;
    }
}

//----find the initial log_addr----------------
//--- to store checkpoint----------------------
//---note: start finding since seg which is next--
//---------to tail_log_addr->seg_no---
//--- should be continuous address------------
void get_cp_loc()
{
    //---- check whether need to change checkpoing location--
    if(!need_change_cp_loc())
    {
        return;
    }
    else
    {
        LogAddrList * cp_log_addr_walker = super_seg->cp_addr;
        u_int start_seg_no = tail_log_addr->seg_no;
        while(cp_log_addr_walker != NULL)
        {
            u_int free_seg_no = find_free_seg(start_seg_no);
            cp_log_addr_walker->log_addr.seg_no = free_seg_no;
            cp_log_addr_walker->log_addr.bk_no = 1;

            start_seg_no = free_seg_no;
            cp_log_addr_walker = cp_log_addr_walker->next;
        }
    }
}

//------to be done later ??--------------------
//--- store the last 2 checkpoints, update by on another-----------
//---when read checkpoint, check the checksum, use the most recent-
//--checkpoint that have the correct checksum----------------------
//------ --------------------------------------

//--调用之前: global value checkpoint已经在内存中------
//------checkpoint addr is in super seg------------------
//-------find continuous bks of checkpoint_size, --------
//-----use this continuous space store checkpoint-------
void store_checkpoint()
{
    //----找到存cp的segs---------------
    //note: 若需要移动cp到新的地址意味着原来存cp的seg worn out了---
    //----因此也不需要设置原来的seg的ischeckpoint属性为flase---- 
    get_cp_loc();
    u_int checkpoint_size = super_seg->checkpoint_size;
    
    //----------把cp数据存在buffer里，然后按seg 分割存入每个seg---
    //--存checkpoint的 seg 的对应seg_usage_table的is_checkpoint属性为true-
    //---拿整数个seg存checkpoint--------------
    //---标记那些存checkpoint的seg不参与cleaning的过程
   
    //---1. store current checkpoint info into buffer-------- 
    void * buffer = calloc(1, checkpoint_size * bk_size * FLASH_SECTOR_SIZE);
    u_int bytes_offset = 0; 
    //--- for checkpoint structure-------------------------
    memcpy(buffer + bytes_offset, checkpoint, sizeof(Checkpoint));
    bytes_offset += sizeof(Checkpoint);
    //---- for ifile of checkpoint--------------------------
    memcpy(buffer + bytes_offset, checkpoint->ifile, sizeof(Inode));
    bytes_offset += sizeof(Inode);
    //---- for seg_usage_table of checkpoint----------------
    int i;
    for(i = 1; i < seg_num; i++)
    {
        Seg_usage_table * sut_walker = checkpoint->seg_usage_table;
        int j = i;
        while(j > 1)
        {
            sut_walker = sut_walker->next;
            j--;
        }
        memcpy(buffer + bytes_offset, sut_walker, sizeof(Seg_usage_table));
        bytes_offset += sizeof(Seg_usage_table);
    }
    
    //---for curr_time of checkpoint------------
    memcpy(buffer + bytes_offset, checkpoint->curr_time, sizeof(time_t));
    bytes_offset += sizeof(time_t);
    //---- for last_log_addr--------------------
    memcpy(buffer + bytes_offset, checkpoint->last_log_addr, sizeof(LogAddress));    
    //---2. store the buffer into right cp location----------
    Flash_Flags flags = FLASH_SILENT;
    u_int tmp = sec_num / FLASH_SECTORS_PER_BLOCK;
    u_int * blocks = &tmp;
    FLash flash = Flash_Open(fl_file, flags, blocks);
    LogAddrList * cp_addr_walker = super_seg->cp_addr;
    
    u_int buffer_offset = 0;
    while(cp_addr_walker != NULL)
    {
        u_int sec_offset = cp_addr_walker->log_addr->seg_no;
        sec_offset = sec_offset * seg_size;
        //---1. fill the seg with relative part of checkpoint------       
        Flash_Write(flash, sec_offset, seg_size, buffer + buffer_offset);
        buffer_offset += seg_size * FLASH_SECTOR_SIZE;  
        
        //----2.In seg usage table --------------------------------
        //----tag the seg storing cp to be is_checkpoint = true ----
        Seg_usage_table * sut_walker = checkpoint->seg_usage_table;
        while(sut_walker->seg_no != cp_addr_walker->log_addr->seg_no)
            sut_walker = sut_walker->next;
        sut_walker->is_checkpoint = true;
        
        cp_addr_walker = cp_addr_walker->next;
    }
}

/*
 *
 *?? means not for sure or need improve in future
 *
 *
 **********/

//wearlimit: 1000, seg:32 bk, b: 2 sector, total sect: 1024
//file: flash name, total_sec: flash size
//suppose: log layer's seg size and block size same as flash layer
int Log_Create()
{
    int i;

    u_int cal_tmp = sizeof(Checkpoint) + sizeof(Inode) 
        + sizeof(Seg_usage_table) * (seg_num - 1) + sizeof(LogAddress);
    u_int checkpoint_size = 0;
    while(checkpoint_size * bk_size * FLASH_SECTOR_SIZE < cal_tmp)
        checkpoint_size ++;
    //---guarantee checkpoint size to be whole number of seg size---
    u_int cp_tmp = checkpoint_size;
    if(checkpoint_size % bks_per_seg != 0)
      checkpoint_size = (checkpoint_size / bks_per_seg) * bks_per_seg
          + bks_per_seg;  

    cal_tmp = sizeof(Begin_bk *) + sizeof(Begin_bk) + sizeof(Seg_sum_bk)
       +  sizeof(Seg_sum_entry) * (seg_num - 1);
    u_int begin_bk_size = 0;
    while(begin_bk_size * bk_size * FLASH_SECTOR_SIZE < cal_tmp)
        begin_bk_size ++;

    //--------------------------------------------------------------
    //-----1st log seg is super log seg----------------------------
    //-------data seg start from 1 ---------------------------
    //-----------[1, segs per log -1]----------------


    //initialize log structure,
    //根据用户输入参数创建flash memory
    //-------------------------------------------    
    //create flash file
    Flash_Create(fl_file, wearlimit, sec_num / FLASH_SECTORS_PER_BLOCK);
    
    //----------------- super  seg      ----------------------------
    //---------------------------------------------------------------
    //format flash file as segment(same size as log seg)
  
    //1.store Super log seg in flash memory's 1st seg
    //1.1 create super log seg 
    //1.2 put super log seg in disk (start from disk first sector)
    //choose the model of Flash
    Flash_Flags flags = FLASH_SILENT;
    
    //blocks : # of blocks in the flash
    u_int tmp = sec_num / FLASH_SECTORS_PER_BLOCK;
    u_int * blocks = &tmp;
    Flash   flash = Flash_Open(fl_file, flags, blocks); 
   
    //------------把super_seg的信息一个一个填进去----------- 
    void * s_seg_buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);

    u_int bytes_offset = 0;
    //----about calloc----------
    Super_seg * s_seg = (Super_seg *)calloc(1, sizeof(Super_seg));
    s_seg->seg_no = 0;
    s_seg->seg_num = seg_num;
    s_seg->seg_size = seg_size;
    s_seg->bk_size = bk_size;
    s_seg->wearlimit = wearlimit;
    s_seg->sec_num = sec_num;
    s_seg->checkpoint_size = checkpoint_size;
    s_seg->begin_bk_size = begin_bk_size;
    LogAddrList * cp_addr = calloc(1, sizeof(LogAddrList));
    cp_addr->log_addr.seg_no = 1;
    cp_addr->log_addr.bk_no = 1;
    cp_addr->next = NULL;
    s_seg->cp_addr = calloc(1, sizeof(LogAddrList));
    memcpy(s_seg->cp_addr, cp_addr, sizeof(LogAddrList));
    free(cp_addr); 
    memcpy(s_seg_buffer, s_seg, sizeof(Super_seg));
    bytes_offset += sizeof(Super_seg);
    
    free(s_seg);

    Flash_Write(flash, 0, seg_size, s_seg_buffer); 

    //-----------------------------------------------------------
    get_slog_to_memory(); 
    //----1. initialize super_seg->cp_addr list-------------
    u_int cp_addr_num = checkpoint_size / bks_per_seg;   
    LogAddrList * cp_log_addr_walker = super_seg->cp_addr;
    u_int free_seg_no = 1;
    while(cp_addr_num > 0)
    {
        cp_log_addr_walker->log_addr.seg_no = free_seg_no;
        cp_log_addr_walker->log_addr.bk_no = 1;

        if(cp_addr_num != 1)
        {
            LogAddrList * tmp_cp_log_addr = (LogAddrList *)calloc(1, sizeof(LogAddrList));
            cp_log_addr_walker->next = tmp_cp_log_addr;
            cp_log_addr_walker = cp_log_addr_walker->next;
        }
        else
            cp_log_addr_walker->next = NULL;

        free_seg_no ++;
        cp_addr_num--;
    }
    //--  2. flag segment storing checkpoint-----
    get_checkpoint_to_memory();
    //--- initialize checkpoint properties---------
    //----------for ifile in checkpoint---------------------
    Inode * tmp_inode = (Inode *)calloc(1, sizeof(Inode));
    tmp_inode->ino = -1;
    tmp_inode->filetype = 0;
    tmp_inode->filesize = 0;
    tmp_inode->filename[0] = 'i';
    tmp_inode->filename[1] = 'f';
    tmp_inode->filename[2] = 'i';
    tmp_inode->filename[3] = 'l';
    tmp_inode->filename[4] = 'e';
    //----??-------------------------------------
    for(i = 0; i < DIRECT_BK_NUM; i++)
    {
        //-----?? seg_no 初始化为 0-------------
        tmp_inode->direct_bk[i].seg_no = 0;
        tmp_inode->direct_bk[i].bk_no = i;
    }
    tmp_inode->indirect_bk.seg_no = 0;
    tmp_inode->indirect_bk.bk_no = i;
    //-----??-----------------------------------
    tmp_inode->mode = 0;
    tmp_inode->userID = getuid();
    tmp_inode->groupID = getgid();
    time_t t;
    time(&t);
    tmp_inode->modify_Time = t;
    tmp_inode->access_Time = t;
    tmp_inode->create_Time = t;
    tmp_inode->change_Time = t;
    tmp_inode->num_links = 0;
    
    memcpy(checkpoint->ifile, tmp_inode, sizeof(Inode));
    free(tmp_inode);

    //-------for seg_usage_table in checkpoint-----------
    Seg_usage_table * sut_walker = checkpoint->seg_usage_table;
    for(i = 1; i < seg_num; i++)
    {
        sut_walker->seg_no = i;
        sut_walker->is_checkpoint = false;
        sut_walker->num_live_bk = 0;
        sut_walker->modify_time = -1;
        sut_walker = sut_walker->next; 
    }
    
    //---- initialize the tail_log_addr--------------------
    tail_log_addr = (LogAddress *)calloc(1, sizeof(LogAddress));
    tail_log_addr->seg_no = free_seg_no;
    tail_log_addr->bk_no = 1;
 
    //-------for last_log_addr in checkpoint--------------
    memcpy(checkpoint->last_log_addr, tail_log_addr, sizeof(LogAddress));

    store_checkpoint();

    //------------create normal seg-----------------------------------
    //----------------  ?? -------------------------------------------
    //----------假设 begin bk 的空间 <= 1 bk ------------------------
    //--- 所以在mklfs.c 文件中设置bks_per_seg不能太大---------------
    for(i = 1; i < seg_num; i++)
    {
        void * n_seg_buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);
        bytes_offset = 0;
        Seg * ts = (Seg *)calloc(1, sizeof(Seg));
        ts->begin_bk = n_seg_buffer + sizeof(Begin_bk *) + sizeof(Block *);
        memcpy(n_seg_buffer + bytes_offset, ts, sizeof(Seg));
        bytes_offset += sizeof(Seg);
        free(ts);

        //--------begin bk----------------------------------
        Begin_bk * bb = (Begin_bk *)calloc(1, sizeof(Begin_bk));
        bb->seg_no = i;
        bb->ssum_bk = n_seg_buffer + bytes_offset + sizeof(int)
            + sizeof(Seg_sum_bk *);
        memcpy(n_seg_buffer + bytes_offset, bb, sizeof(Begin_bk));
        bytes_offset += sizeof(Begin_bk);
        free(bb);
        
        //----seg sum bk of begin bk----------------------------
        Seg_sum_bk * ssb = (Seg_sum_bk *)calloc(1, sizeof(Seg_sum_bk));
        ssb->bk_no = 0;
        ssb->seg_sum_entry = n_seg_buffer + bytes_offset + sizeof(int)
            + sizeof(Seg_sum_entry *);
        memcpy(n_seg_buffer + bytes_offset, ssb, sizeof(Seg_sum_bk));
        bytes_offset += sizeof(Seg_sum_bk);
        free(ssb);
       
        //---- seg sum entry of seg sum bk---------------------- 
        int j;
        for(j = 1; j < bks_per_seg; j++)
        {
            Seg_sum_entry * sse = (Seg_sum_entry *)(n_seg_buffer
                    + bytes_offset);
            sse->bk_no = j;
            sse->file_no = -1;
            sse->file_bk_no = -1;
            bytes_offset += sizeof(Seg_sum_entry);
            if(i != bks_per_seg - 1)
                sse->next = n_seg_buffer + bytes_offset;
            else
                sse->next = NULL;
        }
/*        
        //-------------normal bk-----------------------------
        //---start offset: seg_start + bk_size * FLASH_SECTOR_SIZE
        for(j = 1; j < bks_per_seg; j++)
        {
            Block * b = (Block *)(n_seg_buffer + 
                    j * bk_size * FLASH_SECTOR_SIZE);
            b->bk_content = n_seg_buffer + 
                j * bk_size * FLASH_SECTOR_SIZE + sizeof(void *);
        } 
*/
        Flash_Write(flash, seg_size * i, seg_size, n_seg_buffer); 
        free(n_seg_buffer); 
    }

    Flash_Close(flash);
    
    return 0;
}

//---------------create cache---------------------------------------
//----initially get the first cache_seg_num into memory----------
int create_cache()
{
    Disk_cache *cache_start = (Disk_cache *)calloc(1, sizeof(Disk_cache));
    cache_start->cache_no = 0;
    
    void * tbuffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);
    copy_log_to_memory(1, tbuffer);
    cache_start->seg = (Seg *)tbuffer;

    cache_start->IS_JUST_UPDATE = false;
    cache_start->next = NULL;

    disk_cache = cache_start;

    int i;
    for(i = 1; i < cache_seg_num; i++)
    {
        Disk_cache * tmp = (Disk_cache *)calloc(1, sizeof(Disk_cache));
        tmp->cache_no = i;
        
        void * tb = calloc(1, seg_size * FLASH_SECTOR_SIZE);
        copy_log_to_memory(i + 1, tb);
        cache_start->seg = (Seg *)tb;
        tmp->IS_JUST_UPDATE = false;
        tmp->next = NULL;

        while(cache_start->next != NULL)
            cache_start = cache_start->next;

        cache_start->next = tmp; 
    }
    return 0;   
}

//----------help identify the minimum # of bks --------------
//-------- for the length(in bytes)--------------------------
u_int length_in_bk(u_int len) 
{
    u_int bks_count = 0;
    while(bks_count * bk_size * FLASH_SECTOR_SIZE <= len)
    {
        bks_count++;
    }

    return bks_count;
}

//-----------------read_cache----------------------------------
//------- input: buffer only need to be a pointer
//--------- now file layer only give me length = 1 block data in bytes--------
//---------for file layer simplicity--------------------------------------
//--- now this func can deal with length could be > 1 block data in bytes------
bool read_cache(LogAddress * log_addr, u_int length, void * buffer)
{
    buffer = calloc(1, length);
    u_int offset = 0;

    bool IS_IN_CACHE = false;

    //blocks remain in 1st seg
    u_int bks_remain = bks_per_seg - log_addr->bk_no;
 
    u_int bks_tobe_read = length_in_bk(length);
    
    Disk_cache * cache_walker = disk_cache;

    while(cache_walker != NULL)
    {
        //If the input log_addr's seg no is in the cache
        if(log_addr->seg_no == cache_walker->seg->begin_bk->seg_no)
        {
            //---if all data stored in the same seg--------------
            if(bks_tobe_read <= bks_remain)
            {
                //write data smaller than data size of bks_remain
                memcpy(buffer + offset, cache_walker->seg->begin_bk
                        + log_addr->bk_no * bk_size * FLASH_SECTOR_SIZE, length);
                IS_IN_CACHE = true;
                break;
            }
            else
            {
                memcpy(buffer + offset, cache_walker->seg->begin_bk
                        + log_addr->bk_no * bk_size * FLASH_SECTOR_SIZE,
                        bks_remain * bk_size * FLASH_SECTOR_SIZE);
                offset += bks_remain * bk_size * FLASH_SECTOR_SIZE;
                length = length - bks_remain * bk_size * FLASH_SECTOR_SIZE;
                bks_remain = bks_per_seg;
                bks_tobe_read = length_in_bk(length);
                log_addr->seg_no++;
                log_addr->bk_no = 1;
                cache_walker = disk_cache;
            }

        }
        else
            cache_walker= cache_walker->next;
    }
   
    if(!IS_IN_CACHE)
        free(buffer);

    return IS_IN_CACHE;
}

//----------------------------------Note-------------------------
//-------access flash memory should always in terms of seg-------

//----------help identify the minimum # of segs --------------
//-------- for the length(in bks)--------------------------
u_int length_in_seg(u_int bks_tobe_read) 
{
    u_int segs_count = 0;
    while(segs_count * bks_per_seg  <= bks_tobe_read)
    {
        segs_count++;
    }

    return segs_count;
}

//input: log_addr 地址，返回长度为length的disk数据于buffer中
//-------Here now: length is always = bk_size (in bytes)----------
//-------from file layer-----------------------------------------
//----but this func is able to read > 1 bk_size data--------------
//--- if length > 1 bk_size -----------------------------------
//--------------------不读 Begin block----------------------------
//---------Note: flash memory I/O in unit of segment-------------
int Log_Read(LogAddress * log_addr, u_int length, void * buffer)
{
    //attemp to read from cache
    bool IS_IN_CACHE = read_cache(log_addr, length, buffer);   
    if(IS_IN_CACHE)
        return 0;

    //If not in cache, read data from disk, 
    //then store it on cache from the 1st cache seg
    //----------1. read data from flash-----------
    buffer = calloc(1, length);

    //choose the model of Flash
    Flash_Flags flags = FLASH_SILENT;
    u_int tmp = bks_per_seg * super_seg->seg_num;
    u_int * blocks = &tmp;
    Flash flash = Flash_Open(fl_file, flags, blocks); 
   
    u_int sec_offset = log_addr->seg_no * seg_size;
    u_int bks_remain = bks_per_seg - log_addr->bk_no;
    u_int bks_tobe_read = length_in_bk(length);
    u_int buffer_offset = 0;
    
    bool read_done = false;
    u_int segs_tobe_read = length_in_seg(bks_tobe_read); 
    u_int segs_read[segs_tobe_read];
    
    int i;
    for(i = 0; i < segs_tobe_read; i++)
    {
        segs_read[i] = log_addr->seg_no + i;
    }

    while(!read_done)
    {
        if(bks_tobe_read <= bks_remain)
        {
            void * new_buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);

            //Read the entire seg which contains the data
            //-------data may be > 1 seg size------------------
            Flash_Read(flash, sec_offset, seg_size, new_buffer);

            memcpy(buffer + buffer_offset, new_buffer
                    + log_addr->bk_no * bk_size * FLASH_SECTOR_SIZE, length);
            free(new_buffer);
            read_done = true;
        }
        else
        {
            void * new_buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);

            Flash_Read(flash, sec_offset, seg_size, new_buffer);
            
            memcpy(buffer + buffer_offset, new_buffer
                    + log_addr->bk_no * bk_size * FLASH_SECTOR_SIZE, 
                    bks_remain * bk_size * FLASH_SECTOR_SIZE);
            free(new_buffer); 
            sec_offset += seg_size;

            buffer_offset += bks_remain * bk_size * FLASH_SECTOR_SIZE;
            length = length - bks_remain * bk_size * FLASH_SECTOR_SIZE;
            bks_remain = bks_per_seg;
            bks_tobe_read = length_in_bk(length);
            log_addr->seg_no++;
            log_addr->bk_no = 1;
        }
 
    }

    Flash_Close(flash);

    //----------2. store on cache-----------------
    //-------------2.1 check whether all are just updated----
    //---if all yes, set each's IS_JUST_UPDATE = false;
    for(i = 0; i < segs_tobe_read; i++)
    {
        bool ALL_UPDATED = true;
        Disk_cache  * c_walker = disk_cache;
        while(c_walker != NULL)
        {
            if(!c_walker->IS_JUST_UPDATE)
            {
                ALL_UPDATED = false;
                break;
            }
            c_walker = c_walker->next;
        }

        c_walker = disk_cache;
        if(ALL_UPDATED)
        {
            while(c_walker != NULL)
            {
                c_walker->IS_JUST_UPDATE = false;
                c_walker = c_walker->next;
            }

        }    

        //------------2.2 choose 1st cache seg whose-------------
        //-------IS_JUST_UPDATE = false, then memcpy b--------------------
        c_walker = disk_cache;
        while(c_walker != NULL)
        {
            if(!c_walker->IS_JUST_UPDATE)
            {

                /*                
                //??need calloc before use buffer to this func? 
                void * new_buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);
                //choose the model of Flash
                flags = FLASH_SILENT;
                tmp = bks_per_seg * super_seg->seg_num;
                blocks = &tmp;
                flash = Flash_Open(fl_file, flags, blocks); 
                Flash_Read(flash, segs_read[i] * seg_size, seg_size, new_buffer);
                Flash_Close(flash);
                 */              
                void * tbuffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);
                copy_log_to_memory(segs_read[i], tbuffer);

                free(c_walker->seg);

                c_walker->seg = (Seg *)tbuffer;

                c_walker->IS_JUST_UPDATE = true;
                break;
            }
        }

    }

    return 0;

}

//----------copy seg into memory for cache use -----------
//---------- to be written data---------------------------
void copy_log_to_memory(int seg_no, void * copy_seg)
{
    void * buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);
    //choose the model of Flash
    Flash_Flags flags = FLASH_SILENT;
    u_int tmp = bks_per_seg * seg_num;
    u_int * blocks = &tmp;
    Flash flash = Flash_Open(fl_file, flags, blocks); 
    Flash_Read(flash, seg_no * seg_size, seg_size, buffer);

    Flash_Close(flash);

    //--------reconstruct the normal segment to memory from disk----------    
    u_int bytes_offset = 0;
    Seg* tseg = (Seg *)buffer;
    tseg->begin_bk = (Begin_bk *)calloc(1, sizeof(Begin_bk));
    tseg->begin_bk->seg_no = seg_no;
    tseg->begin_bk->ssum_bk =
        (Seg_sum_bk *)calloc(1, sizeof(Seg_sum_bk));
    
    bytes_offset += sizeof(Seg) + sizeof(Begin_bk);
    tseg->begin_bk->ssum_bk->bk_no = 0;
    bytes_offset += sizeof(Seg_sum_bk);
    
    Seg_sum_entry * sse_walker = (Seg_sum_entry *)calloc(1, sizeof(Seg_sum_entry));
    tseg->begin_bk->ssum_bk->seg_sum_entry = sse_walker;
 
    int i;
    for(i = 1; i < bks_per_seg; i++)
    {
        Seg_sum_entry * sse = (Seg_sum_entry *)(buffer + bytes_offset);
        
        bytes_offset += sizeof(Seg_sum_entry);
        if(i != bks_per_seg - 1)
            sse->next = buffer + bytes_offset;
        else
            sse->next = NULL;

        memcpy(sse_walker, sse, sizeof(Seg_sum_entry));
//--?? 可能memory leak----------
//         sse_walker = sse;
        sse_walker = sse_walker->next;
    }

    memcpy(copy_seg, tseg, seg_size * FLASH_SECTOR_SIZE);
    free(tseg);
}


//----------grab one seg from flash memory into memory-------
//---------- to be written data----------------------------
//-- serve for log write func-------------------------------
void get_log_to_memory(LogAddress * log_addr)
{
    void * buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);
    //choose the model of Flash
    Flash_Flags flags = FLASH_SILENT;
    u_int tmp = bks_per_seg * seg_num;
    u_int * blocks = &tmp;
    Flash flash = Flash_Open(fl_file, flags, blocks); 
    Flash_Read(flash, log_addr->seg_no * seg_size, seg_size, buffer);

    Flash_Close(flash);

    if(seg_in_memory != NULL)
        free(seg_in_memory);
    
    //--------reconstruct the segment to memory from disk-------------    
    u_int bytes_offset = 0;
    seg_in_memory = (Seg *)buffer;
    seg_in_memory->begin_bk = (Begin_bk *)calloc(1, sizeof(Begin_bk));
    seg_in_memory->begin_bk->seg_no = log_addr->seg_no;
    seg_in_memory->begin_bk->ssum_bk =
        (Seg_sum_bk *)calloc(1, sizeof(Seg_sum_bk));
    
    bytes_offset += sizeof(Seg) + sizeof(Begin_bk);
    seg_in_memory->begin_bk->ssum_bk->bk_no = 0;
    bytes_offset += sizeof(Seg_sum_bk);
    
    Seg_sum_entry * sse_walker = (Seg_sum_entry *)calloc(1, sizeof(Seg_sum_entry));
    seg_in_memory->begin_bk->ssum_bk->seg_sum_entry = sse_walker;
 
    int i;
    for(i = 1; i < bks_per_seg; i++)
    {
        Seg_sum_entry * sse = (Seg_sum_entry *)(buffer + bytes_offset);
        
        bytes_offset += sizeof(Seg_sum_entry);
        if(i != bks_per_seg - 1)
            sse->next = buffer + bytes_offset;
        else
            sse->next = NULL;

        memcpy(sse_walker, sse, sizeof(Seg_sum_entry));
//--?? 可能memory leak----------
//         sse_walker = sse;
        sse_walker = sse_walker->next;
    }

}

void get_slog_to_memory()
{
    void * buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);
    //choose the model of Flash
    Flash_Flags flags = FLASH_SILENT;
    u_int tmp = sec_num / FLASH_SECTORS_PER_BLOCK;
    u_int * blocks = &tmp;
    Flash flash = Flash_Open(fl_file, flags, blocks); 
    Flash_Read(flash, 0, seg_size, buffer);
    Flash_Close(flash);
    
    //--------reconstruct the segment to memory from disk-------------    
    u_int bytes_offset = 0;
    super_seg = (Super_seg *)buffer;

    //--for cp_addr in super_seg--------------------------------------
    bytes_offset += sizeof(Super_seg);
    LogAddrList * la_walker = (LogAddrList *)calloc(1, sizeof(LogAddrList));
    super->cp_addr = la_walker;
    while(la_walker != NULL)
    {
        LogAddrList * la = (LogAddrList *)(buffer + bytes_offset);
        bytes_offset += sizeof(LogAddrList);
        memcpy(la_walker, la, sizeof(LogAddrList));
        la_walker = la_walker->next;
    }
}

//--- given log addr, check whether this addr is the last addr---
//-- within that seg that is <= wearlimit and empty--------------
bool is_remain_seg_not_usable(LogAddress * log_addr)
{
    bool returnValue = true;
    
    int i = log_addr->bk_no;
    if (i == bks_per_seg - 1)
    {
        ;
    }
    else
    {
        i++;
        LogAddress * tmp_addr = (LogAddress *)calloc(1, sizeof(LogAddress));
        for(; i < bks_per_seg; i++)
        {
            tmp_addr->seg_no = log_addr->seg_no;
            tmp_addr->bk_no = i;
            if(is_in_wearlimit(tmp_addr))
            {
                returnValue = false;
                break;
            }
        }
    }

    return returnValue;
}


//--- check the seg_usage_table's is_checkpoint property--
bool is_cp_seg(LogAddress * log_addr)
{
    Seg_usage_table * sut_walker = checkpoint->seg_usage_table;
    
    int seg_no = log_addr->seg_no;
    int i = 1;
    while(i < seg_no)
    {
        sut_walker = sut_walker->next;
        i++;
    } 
    
    return sut_walker->is_checkpoint; 
}


//-------prerequisite: know the log_addr->seg_no has good bk to be used-----
//--- find available bk includes log_addr->bk itself-----------------------
void locate_tail_log_addr_bk(LogAddress * log_addr)
{
    LogAddress * tmp_addr = (LogAddress *)calloc(1, sizeof(LogAddress));
    int i = log_addr->bk_no;
    for(; i < bks_per_seg; i++)
    {
           tmp_addr->seg_no = log_addr->seg_no;
           tmp_addr->bk_no = i;
           if(is_in_wearlimit(tmp_addr))
           {
               tail_log_addr->seg_no = log_addr->seg_no;
               tail_log_addr->bk_no = i;
               return;
           }
    }
}


//--------locate tail_log_addr from first normal segment----------------
void locate_tail_log_addr_from_begin()
{
    int i;
    for(i = 1; i < seg_num; i++)
    {
        //--- check seg_usage_table's is_checkpoint ----
        Seg_usage_table * sut_walker = checkpoint->seg_usage_table;
        int j = i;
        while(j > 1)
        {
            sut_walker = sut_walker->next;
            j--;
        }

        //---- if the seg is not seg storing cp---------
        if(sut_walker->is_checkpoint == false)
        {
            tmp_addr->seg_no = i;
            tmp_addr->bk_no = 1;
            //---- if this seg has available bk to store data---
            if(!is_remain_seg_not_usable(tmp_addr))
            {
                locate_tail_log_addr_bk(tmp_addr);
                break;
            } 
        }
    }
}


//--------  find the next bk < wearlimit and available--------
//-------------assist func for log write-------------------------
//-----------set & grab a new log seg if necessary-----------
//--------call this func after write data to log----------------
void setLogTail()
{
    LogAddress * tmp_addr = (LogAddress *)calloc(1, sizeof(LogAddress));
    //If come to end of certain log seg
    if(is_remain_seg_not_usable(tail_log_addr))
    {
        //If come to every end of log structure
        if(tail_log_addr->seg_no == seg_num - 1)
        {
            locate_tail_log_addr_from_begin();
        }
        //else turn to check next log seg's 1th bk
        else
        {
           bool can_find_in_back = true;
           tmp_addr->seg_no = tail_log_addr->seg_no + 1;
           tmp_addr->bk_no = 1;
           //-------------------------??-----------
           for()
        }

        get_log_to_memory(tail_log_addr);
    }
    //find usable bk within that seg
    else
    {
        tmp_addr->seg_no = tail_log_addr->seg_no;
        tmp_addr->bk_no = tail_log_addr->bk_no + 1;
        locate_tail_log_addr_bk(tmp_addr);
    }

    free(tmp_addr);
}

//---??add check if for the now using seg, the remaining bks are all
// >= wearlimit or not, if yes push to disk and add the wearlimit

//---------once tail_log_addr reaches certain log seg's end--------
//-------- push that log seg data into disk------------------------
//---------call this func before setLogTail()-------------------
void pushToDisk(LogAddress * log_addr)
{
    //If reaches certain log seg's end, write entire log seg to disk
    if(log_addr->bk_no == bks_per_seg -1)
    {
        //1.write to disk
        //choose the model of Flash
        Flash_Flags flags = FLASH_SILENT;

        //blocks : # of blocks in the flash
        u_int tmp = bks_per_seg * seg_num;
        u_int * blocks = &tmp;
        Flash   flash = Flash_Open(fl_file, flags, blocks);
        Flash_Write(flash, tail_log_addr->seg_no * seg_size, 
                seg_size, seg_in_memory);
        Flash_Close(flash);
    }
}

//-------------------write data log's one block---------------
//------ now   always   write 1 block size in bytes---------------
//-------each time call the log_write func----------------
void writeToLog(int inum, int block, void * buffer, LogAddress * log_addr)
{
    //---change the log_addr's content------------
    memcpy(seg_in_memory + bk_size * log_addr->bk_no * FLASH_SECTOR_SIZE,
            buffer, bk_size * FLASH_SECTOR_SIZE);

    Seg_sum_entry * sse_walker = seg_in_memory->begin_bk->ssum_bk->seg_sum_entry;
    //----change the seg_in_memory's Begin_bk---------
    while(sse_walker != NULL)
    {
        if(log_addr->bk_no == sse_walker->bk_no)
        {
            sse_walker->file_no = inum;
            sse_walker->file_bk_no = block;
            
            break;
        }
        sse_walker = sse_walker->next; 
    }

    //-------update seg usage table------------------
    int i;
    Seg_usage_table * sut_walker = super_seg->seg_usage_table;
    for(i = 0; i < seg_num; i++)
    {
        if(sut_walker->seg_no == log_addr->seg_no)
        {
            sut_walker->num_live_bk += 1;
            
//?? modify_time 怎么搞
            break;
        }
    }
}

//------------------------------------------------------------------




//-----------------------------------------------------------------
//---------将文件的inum(inode)的第block号块写入log,----------------
//------- 写入log的地址为tail_log_addr ---------------------------
//----------------------input--------------------------------------
//--------input: length - always = fl_bk_size;
//-----------!!! 一次只写一个 block的数据--------------------------
//--------input: block - bk no within the file
int Log_Write(int inum, int block, u_int length, 
        void * buffer, LogAddress * log_addr)
{
    writeToLog(inum, block, buffer, log_addr);
    pushToDisk(log_addr);
    setLogTail();

    return 0;
}


//-------- erase blocks-----------------------------
//--------- in unit of  16  sectors-------------------------
//-------input: log_addr can only at 16 whole number location------
int Log_Free(LogAddress * log_addr, u_int length)
{

    u_int erase_bks = 0;
    while(erase_bks * FLASH_SECTORS_PER_BLOCK * FLASH_SECTOR_SIZE <= length)
        erase_bks++;


    u_int offset = (log_addr->seg_no * seg_size + log_addr->bk_no * bk_size)
        / FLASH_SECTORS_PER_BLOCK;



    //choose the model of Flash
    Flash_Flags flags = FLASH_SILENT;

    //blocks : # of blocks in the flash
    u_int tmp = bks_per_seg * seg_num;
    u_int * blocks = &tmp;
    Flash   flash = Flash_Open(fl_file, flags, blocks);
    Flash_Erase(flash, offset, erase_bks);
    Flash_Close(flash);

    return 0;
}

//-----此func目前未被调用过---------------------------------
//---------------------------------------------------------
int Log_Init(char * filename, Inode * iifile, u_int cachesize)
{
    void * buffer = calloc(1, seg_size * FLASH_SECTOR_SIZE);
    //--- 2.  read super seg------------------------------- 
    Flash_Flags flags = FLASH_SILENT;

    //blocks : # of blocks in the flash
    u_int tmp = sec_num / FLASH_SECTORS_PER_BLOCK;
    u_int * blocks = &tmp;
    Flash   flash = Flash_Open(filename, flags, blocks);
    Flash_Read(flash, 0, seg_size, buffer);
//----------------?? 为毛filename为空---------------
    Inode * tf = (Inode *)(buffer + sizeof(Super_seg) + (seg_num - 1) * sizeof(Seg_usage_table) + sizeof(Checkpoint));

    memcpy(iifile, tf, sizeof(Inode));
    free(buffer);

    return 0;
}

